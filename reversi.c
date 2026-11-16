/*
 * =====================================================================
 *  REVERSI (OTHELLO) EM REDE  -  Trabalho 02 - Sistemas Operacionais
 *  Jogo para 2 jogadores, cada um em um processo, comunicando por
 *  SOCKET (TCP) com um protocolo de mensagens proprio.
 * =====================================================================
 *
 *  COMPILAR
 *    Windows (MinGW):  gcc reversi.c -o reversi.exe -lws2_32 -lpthread
 *    Linux / macOS  :  gcc reversi.c -o reversi -lpthread
 *
 *  EXECUTAR (um processo em cada terminal)
 *    reversi servidor [porta]          -> jogador 1 (pecas pretas, X)
 *    reversi cliente  <ip> [porta]     -> jogador 2 (pecas brancas, O)
 *    (sem argumentos, o programa pergunta o que fazer)
 *    Opcoes extras: --nome <nome>   --bot <N partidas>
 *
 *  ARQUITETURA (resumo - detalhes no README.md)
 *    - Cada processo tem UMA copia do estado do jogo (struct Estado),
 *      alocada na heap e COMPARTILHADA entre duas threads:
 *          thread principal  : le o teclado e envia as jogadas
 *          thread receptora  : recebe mensagens do oponente
 *    - Essa memoria compartilhada e protegida por MUTEX
 *      (mtx) + VARIAVEL DE CONDICAO (cv).
 *    - Os dois processos mantem as copias iguais trocando mensagens.
 *      Cada mensagem de jogada leva a "vez" seguinte e um checksum do
 *      tabuleiro, entao qualquer divergencia e detectada.
 *    - O historico de jogadas e um vetor dinamico (malloc/realloc/free).
 *
 *  PROTOCOLO  (1 mensagem = 1 linha de texto terminada em '\n')
 *      <jogador><TIPO><argumentos>
 *      jogador = '1' ou '2' (quem ENVIOU a mensagem)
 *
 *      1Nfulano            N = nome (handshake inicial)
 *      1Jd3;2;9F3A12BC     J = jogada na casa d3; depois dela a vez e do
 *                              jogador 2 (0 = fim de jogo); checksum
 *                              hexadecimal do tabuleiro apos a jogada
 *      2Coi, tudo bem?     C = chat
 *      1D                  D = desistencia
 *      1R                  R = quero revanche
 *      2E3                 E = erro (codigo) ao validar mensagem recebida
 *      1Q                  Q = estou saindo
 * =====================================================================
 */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0601
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALIDO   INVALID_SOCKET
  #define fechar_sock     closesocket
  #define DESLIGAR_AMBOS  SD_BOTH
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
  #include <errno.h>
  typedef int sock_t;
  #define SOCK_INVALIDO   (-1)
  #define fechar_sock     close
  #define DESLIGAR_AMBOS  SHUT_RDWR
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/*  Constantes e tipos                                              */
/* ---------------------------------------------------------------- */
#define N              8       /* tabuleiro 8x8                      */
#define PORTA_PADRAO   5000
#define NOME_MAX       24
#define LINHA_MAX      512
#define CHAT_MAX       200
#define TELA_MAX       2048

enum { FASE_LOBBY, FASE_JOGANDO, FASE_FIM };
enum { OK = 0, ERR_FASE, ERR_VEZ, ERR_INVALIDA, ERR_MEM };

typedef struct {
    unsigned char jog, l, c, viradas;    /* quem, onde, quantas pecas virou */
} Jogada;

/* Estado do jogo: a "memoria compartilhada" entre as threads. */
typedef struct {
    char tab[N][N];            /* 0 vazio, 1 preto (X), 2 branco (O)  */
    int  vez;                  /* de quem e a vez: 1, 2 (0 = ninguem) */
    int  fase;                 /* FASE_LOBBY / JOGANDO / FIM          */
    int  meu_id;               /* 1 = servidor, 2 = cliente           */
    char nome[3][NOME_MAX];
    int  nome_enviado, nome_ok_opp;
    int  vitorias[3], empates, partidas;
    int  vencedor;             /* -1 nenhum, 0 empate, 1 ou 2         */
    int  desistente;           /* 0 ou id de quem desistiu            */
    int  passou;               /* quem ficou sem jogada na ultima vez */
    int  revanche[3];          /* pedidos de revanche                 */
    int  encerrar;             /* 1 = fim do programa / conexao caiu  */
    int  dessincronizado;      /* 1 = copias do jogo divergiram       */
    Jogada *hist;              /* historico dinamico (heap)           */
    int  hist_n, hist_cap;
    sock_t sock;
    pthread_mutex_t mtx;       /* protege TUDO acima                  */
    pthread_cond_t  cv;        /* acorda quem espera mudanca de estado */
} Estado;

/* Mutexes globais auxiliares (ordem de travamento: mtx -> nunca outro
 * mutex dentro dele; mtx_tela e mtx_envio sao "folhas", nunca travados
 * um dentro do outro, entao nao ha deadlock). */
static pthread_mutex_t mtx_tela  = PTHREAD_MUTEX_INITIALIZER; /* stdout */
static pthread_mutex_t mtx_envio = PTHREAD_MUTEX_INITIALIZER; /* send() */
static int g_interativo = 1;           /* 0 quando roda como bot      */

/* ---------------------------------------------------------------- */
/*  Saida na tela (atomica: uma mensagem nunca se mistura com outra) */
/* ---------------------------------------------------------------- */
static void msg(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&mtx_tela);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&mtx_tela);
}

static void prompt(void)
{
    if (!g_interativo) return;
    pthread_mutex_lock(&mtx_tela);
    fputs("> ", stdout);
    fflush(stdout);
    pthread_mutex_unlock(&mtx_tela);
}

static void erro_rede(const char *onde)
{
#ifdef _WIN32
    msg("[erro] %s (codigo Winsock %d)\n", onde, WSAGetLastError());
#else
    msg("[erro] %s: %s\n", onde, strerror(errno));
#endif
}

/* Deixa so caracteres imprimiveis (evita sequencias de controle). */
static void copiar_limpo(char *dst, const char *src, size_t max)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < max; i++)
        if ((unsigned char)src[i] >= 32 && src[i] != 127) dst[j++] = src[i];
    dst[j] = '\0';
}

/* ---------------------------------------------------------------- */
/*  Regras do Reversi (funcoes puras, sem rede e sem threads)        */
/* ---------------------------------------------------------------- */
static const int DL[8] = {-1, -1, -1,  0, 0,  1, 1, 1};
static const int DC[8] = {-1,  0,  1, -1, 1, -1, 0, 1};

static char simbolo(int v) { return v == 1 ? 'X' : v == 2 ? 'O' : '.'; }

/* Quantas pecas adversarias seriam viradas partindo de (l,c) na direcao
 * (dl,dc): sequencia de pecas do oponente terminada por uma peca minha. */
static int viradas_dir(char tab[N][N], int l, int c, int dl, int dc, int jog)
{
    int n = 0, r = l + dl, k = c + dc;
    while (r >= 0 && r < N && k >= 0 && k < N && tab[r][k] == 3 - jog) {
        n++; r += dl; k += dc;
    }
    if (n > 0 && r >= 0 && r < N && k >= 0 && k < N && tab[r][k] == jog)
        return n;
    return 0;
}

static int total_viradas(char tab[N][N], int l, int c, int jog)
{
    int t = 0;
    if (tab[l][c] != 0) return 0;
    for (int d = 0; d < 8; d++) t += viradas_dir(tab, l, c, DL[d], DC[d], jog);
    return t;
}

static void virar(char tab[N][N], int l, int c, int jog)
{
    for (int d = 0; d < 8; d++) {
        int n = viradas_dir(tab, l, c, DL[d], DC[d], jog);
        for (int i = 1; i <= n; i++) tab[l + DL[d] * i][c + DC[d] * i] = (char)jog;
    }
    tab[l][c] = (char)jog;
}

static int tem_jogada(char tab[N][N], int jog)
{
    for (int l = 0; l < N; l++)
        for (int c = 0; c < N; c++)
            if (total_viradas(tab, l, c, jog) > 0) return 1;
    return 0;
}

static void contar(char tab[N][N], int *p1, int *p2)
{
    *p1 = *p2 = 0;
    for (int l = 0; l < N; l++)
        for (int c = 0; c < N; c++) {
            if (tab[l][c] == 1) (*p1)++;
            else if (tab[l][c] == 2) (*p2)++;
        }
}

/* Checksum (hash tipo djb2) do tabuleiro + vez: vai em toda mensagem de
 * jogada para provar que as duas copias do jogo continuam identicas. */
static uint32_t checksum(char tab[N][N], int vez)
{
    uint32_t h = 5381;
    for (int l = 0; l < N; l++)
        for (int c = 0; c < N; c++) h = h * 33u + (uint32_t)tab[l][c];
    return h * 33u + (uint32_t)vez;
}

/* ---------------------------------------------------------------- */
/*  Operacoes sobre o Estado  (chamar com E->mtx TRAVADO)            */
/* ---------------------------------------------------------------- */
static void nova_partida(Estado *E)
{
    memset(E->tab, 0, sizeof E->tab);
    E->tab[3][3] = 2; E->tab[4][4] = 2;       /* d4 e e5 brancas */
    E->tab[3][4] = 1; E->tab[4][3] = 1;       /* e4 e d5 pretas  */
    E->vez = 1;                               /* pretas comecam  */
    E->fase = FASE_JOGANDO;
    E->hist_n = 0;                            /* reaproveita o vetor */
    E->vencedor = -1;
    E->desistente = 0;
    E->passou = 0;
    E->revanche[1] = E->revanche[2] = 0;
    E->partidas++;
}

/* Comeca a partida quando os dois nomes ja foram trocados. */
static int tentar_iniciar(Estado *E)
{
    if (E->fase == FASE_LOBBY && E->nome_enviado && E->nome_ok_opp) {
        nova_partida(E);
        return 1;
    }
    return 0;
}

/* Recomeca quando os DOIS jogadores pediram revanche. */
static int tentar_revanche(Estado *E)
{
    if (E->fase == FASE_FIM && E->revanche[1] && E->revanche[2]) {
        nova_partida(E);
        return 1;
    }
    return 0;
}

static void finalizar(Estado *E, int desistente)
{
    int p1, p2;
    contar(E->tab, &p1, &p2);
    E->fase = FASE_FIM;
    E->vez = 0;
    E->desistente = desistente;
    if (desistente)  E->vencedor = 3 - desistente;
    else             E->vencedor = p1 > p2 ? 1 : p2 > p1 ? 2 : 0;
    if (E->vencedor) E->vitorias[E->vencedor]++;
    else             E->empates++;
}

/* Garante espaco no historico: vetor dinamico que dobra de tamanho. */
static int hist_garantir(Estado *E)
{
    if (E->hist_n < E->hist_cap) return 1;
    int nova = E->hist_cap ? E->hist_cap * 2 : 16;
    Jogada *p = realloc(E->hist, (size_t)nova * sizeof *p);
    if (!p) return 0;
    E->hist = p;
    E->hist_cap = nova;
    return 1;
}

/* Valida e aplica UMA jogada. Usada tanto para jogadas locais quanto
 * recebidas do oponente: as duas copias passam pelo mesmo codigo. */
static int aplicar_jogada(Estado *E, int jog, int l, int c)
{
    if (E->fase != FASE_JOGANDO)           return ERR_FASE;
    if (E->vez != jog)                     return ERR_VEZ;
    if (l < 0 || l >= N || c < 0 || c >= N) return ERR_INVALIDA;
    int n = total_viradas(E->tab, l, c, jog);
    if (n == 0)                            return ERR_INVALIDA;
    if (!hist_garantir(E))                 return ERR_MEM;

    virar(E->tab, l, c, jog);
    E->hist[E->hist_n].jog = (unsigned char)jog;
    E->hist[E->hist_n].l = (unsigned char)l;
    E->hist[E->hist_n].c = (unsigned char)c;
    E->hist[E->hist_n].viradas = (unsigned char)n;
    E->hist_n++;

    /* Define a proxima vez (regra do Reversi: quem nao tem jogada passa).
     * Como as duas copias calculam igual, "passar" nao precisa de mensagem. */
    E->passou = 0;
    int prox = 3 - jog;
    if (tem_jogada(E->tab, prox))       E->vez = prox;
    else if (tem_jogada(E->tab, jog)) { E->vez = jog; E->passou = prox; }
    else                                finalizar(E, 0);
    return OK;
}

/* ---------------------------------------------------------------- */
/*  Desenho do tabuleiro (texto)  - chamar com E->mtx TRAVADO         */
/* ---------------------------------------------------------------- */
static void ap(char *out, size_t n, size_t *p, const char *fmt, ...)
{
    if (*p >= n) return;
    va_list a;
    va_start(a, fmt);
    int w = vsnprintf(out + *p, n - *p, fmt, a);
    va_end(a);
    if (w > 0) *p += (size_t)w;
    if (*p >= n) *p = n - 1;
}

static void montar_tela(Estado *E, char *out, size_t n)
{
    size_t p = 0;
    int p1, p2;
    int dica = (E->fase == FASE_JOGANDO && E->vez == E->meu_id);
    out[0] = '\0';

    ap(out, n, &p, "\n    a b c d e f g h\n");
    for (int l = 0; l < N; l++) {
        ap(out, n, &p, " %d  ", l + 1);
        for (int c = 0; c < N; c++) {
            char ch = simbolo(E->tab[l][c]);
            if (E->tab[l][c] == 0 && dica && total_viradas(E->tab, l, c, E->meu_id) > 0)
                ch = '*';
            ap(out, n, &p, "%c ", ch);
        }
        ap(out, n, &p, "\n");
    }
    contar(E->tab, &p1, &p2);
    ap(out, n, &p, "\n Placar: %s (X) %d x %d %s (O)   | Vitorias: %d x %d, empates: %d\n",
       E->nome[1], p1, p2, E->nome[2], E->vitorias[1], E->vitorias[2], E->empates);

    if (E->hist_n > 0) {
        Jogada *j = &E->hist[E->hist_n - 1];
        ap(out, n, &p, " Ultima jogada: %s %c%d (virou %d)\n",
           E->nome[j->jog], 'a' + j->c, j->l + 1, j->viradas);
    }
    if (E->passou)
        ap(out, n, &p, " %s nao tinha jogada valida e passou a vez.\n", E->nome[E->passou]);

    if (E->fase == FASE_JOGANDO) {
        ap(out, n, &p, " Vez de: %s (%c)%s\n", E->nome[E->vez], simbolo(E->vez),
           E->vez == E->meu_id ? "  <-- SUA VEZ (ex: d3; * = jogadas possiveis)" : "");
    } else if (E->fase == FASE_FIM) {
        ap(out, n, &p, "\n ===== FIM DE JOGO =====\n");
        if (E->desistente)
            ap(out, n, &p, " %s desistiu. Vencedor: %s\n",
               E->nome[E->desistente], E->nome[E->vencedor]);
        else if (E->vencedor == 0)
            ap(out, n, &p, " Empate: %d x %d\n", p1, p2);
        else
            ap(out, n, &p, " Vencedor: %s (%c) por %d x %d\n",
               E->nome[E->vencedor], simbolo(E->vencedor), p1, p2);
        ap(out, n, &p, " Digite /revanche para jogar de novo ou /sair.\n");
    }
}

/* ---------------------------------------------------------------- */
/*  Rede: envio                                                      */
/* ---------------------------------------------------------------- */
/* Envia uma linha. O mutex de envio evita que duas threads misturem
 * mensagens no mesmo socket (ex.: chat da principal + erro da receptora). */
static int enviar_linha(Estado *E, const char *fmt, ...)
{
    char buf[LINHA_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n > sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';

    int off = 0, rc = 0;
    pthread_mutex_lock(&mtx_envio);
    while (off < n) {
        int k = (int)send(E->sock, buf + off, n - off, 0);
        if (k <= 0) { rc = -1; break; }
        off += k;
    }
    pthread_mutex_unlock(&mtx_envio);
    return rc;
}

/* ---------------------------------------------------------------- */
/*  Acoes do jogador local (usadas pelo teclado e pelo bot)          */
/* ---------------------------------------------------------------- */
static int jogar_local(Estado *E, int l, int c)
{
    char tela[TELA_MAX];
    int rc, vez_apos = 0;
    uint32_t cs = 0;

    pthread_mutex_lock(&E->mtx);
    rc = aplicar_jogada(E, E->meu_id, l, c);
    if (rc == OK) {
        vez_apos = E->vez;
        cs = checksum(E->tab, E->vez);
        montar_tela(E, tela, sizeof tela);
    }
    pthread_cond_broadcast(&E->cv);
    pthread_mutex_unlock(&E->mtx);

    if (rc != OK) {
        switch (rc) {
        case ERR_FASE:     msg("A partida nao esta em andamento.\n"); break;
        case ERR_VEZ:      msg("Nao e a sua vez. Aguarde o oponente.\n"); break;
        case ERR_INVALIDA: msg("Jogada invalida: precisa virar ao menos uma peca do adversario.\n"); break;
        default:           msg("Erro de memoria ao registrar a jogada.\n"); break;
        }
        return rc;
    }
    if (enviar_linha(E, "%dJ%c%d;%d;%08X", E->meu_id, 'a' + c, l + 1, vez_apos, (unsigned)cs) < 0)
        msg("[rede] falha ao enviar a jogada.\n");
    msg("%s", tela);
    return OK;
}

static void desistir_local(Estado *E)
{
    char tela[TELA_MAX];
    int ok = 0;
    pthread_mutex_lock(&E->mtx);
    if (E->fase == FASE_JOGANDO) {
        finalizar(E, E->meu_id);
        montar_tela(E, tela, sizeof tela);
        ok = 1;
    }
    pthread_cond_broadcast(&E->cv);
    pthread_mutex_unlock(&E->mtx);
    if (!ok) { msg("Nao ha partida em andamento.\n"); return; }
    enviar_linha(E, "%dD", E->meu_id);
    msg("%s", tela);
}

static void revanche_local(Estado *E)
{
    char tela[TELA_MAX];
    int estado = 0;            /* 0 = fora de hora, 1 = aguardando, 2 = iniciou */
    pthread_mutex_lock(&E->mtx);
    if (E->fase == FASE_FIM) {
        E->revanche[E->meu_id] = 1;
        estado = 1;
        if (tentar_revanche(E)) { estado = 2; montar_tela(E, tela, sizeof tela); }
    }
    pthread_cond_broadcast(&E->cv);
    pthread_mutex_unlock(&E->mtx);

    if (estado == 0) { msg("A revanche so pode ser pedida quando a partida terminar.\n"); return; }
    enviar_linha(E, "%dR", E->meu_id);
    if (estado == 2) msg("\nNova partida!%s", tela);
    else             msg("Revanche pedida. Aguardando o oponente...\n");
}

static void mostrar_tabuleiro(Estado *E)
{
    char tela[TELA_MAX];
    pthread_mutex_lock(&E->mtx);
    montar_tela(E, tela, sizeof tela);
    pthread_mutex_unlock(&E->mtx);
    msg("%s", tela);
}

static void mostrar_historico(Estado *E)
{
    char out[TELA_MAX];
    size_t p = 0;
    out[0] = '\0';
    pthread_mutex_lock(&E->mtx);
    ap(out, sizeof out, &p, "\nHistorico (%d jogadas):\n", E->hist_n);
    int ini = E->hist_n > 20 ? E->hist_n - 20 : 0;       /* ultimas 20 */
    for (int i = ini; i < E->hist_n; i++) {
        Jogada *j = &E->hist[i];
        ap(out, sizeof out, &p, " %2d. %-12s (%c) %c%d  +%d\n", i + 1,
           E->nome[j->jog], simbolo(j->jog), 'a' + j->c, j->l + 1, j->viradas);
    }
    pthread_mutex_unlock(&E->mtx);
    msg("%s", out);
}

static void ajuda(void)
{
    msg("\nComandos:\n"
        "  d3                jogar na casa d3 (coluna a-h, linha 1-8)\n"
        "  /chat <texto>     enviar mensagem ao oponente   (atalho: /c)\n"
        "  /tab              mostrar o tabuleiro de novo\n"
        "  /hist             mostrar as ultimas jogadas\n"
        "  /desistir         desistir da partida\n"
        "  /revanche         pedir nova partida (apos o fim)\n"
        "  /sair             encerrar o programa\n"
        "  /ajuda            esta ajuda\n\n");
}

/* ---------------------------------------------------------------- */
/*  Thread receptora: le o socket e atualiza o estado compartilhado   */
/* ---------------------------------------------------------------- */
static void processar_msg(Estado *E, char *linha)
{
    int opp = 3 - E->meu_id;
    char tela[TELA_MAX];

    if (strlen(linha) < 2 || linha[0] != (char)('0' + opp)) {
        msg("[rede] mensagem com remetente invalido ignorada.\n");
        return;
    }
    char tipo = linha[1];
    char *arg = linha + 2;

    switch (tipo) {
    case 'N': {                                   /* nome do oponente */
        int iniciou;
        pthread_mutex_lock(&E->mtx);
        copiar_limpo(E->nome[opp], arg, NOME_MAX);
        if (!E->nome[opp][0]) snprintf(E->nome[opp], NOME_MAX, "Jogador%d", opp);
        E->nome_ok_opp = 1;
        iniciou = tentar_iniciar(E);
        if (iniciou) montar_tela(E, tela, sizeof tela);
        pthread_cond_broadcast(&E->cv);
        pthread_mutex_unlock(&E->mtx);
        if (iniciou) { msg("\nPartida iniciada! Digite /ajuda para ver os comandos.%s", tela); prompt(); }
        break;
    }
    case 'J': {                                   /* jogada do oponente */
        char col, lin;
        int vez_msg, rc, dessinc = 0;
        unsigned int cs;
        if (sscanf(arg, "%c%c;%d;%x", &col, &lin, &vez_msg, &cs) != 4) {
            msg("[rede] jogada mal formatada: %s\n", linha);
            enviar_linha(E, "%dE%d", E->meu_id, ERR_INVALIDA);
            break;
        }
        pthread_mutex_lock(&E->mtx);
        rc = aplicar_jogada(E, opp, lin - '1', tolower((unsigned char)col) - 'a');  /* (linha, coluna) */
        if (rc == OK) {
            dessinc = ((uint32_t)cs != checksum(E->tab, E->vez)) || vez_msg != E->vez;
            if (dessinc) E->dessincronizado = 1;
            montar_tela(E, tela, sizeof tela);
        }
        pthread_cond_broadcast(&E->cv);
        pthread_mutex_unlock(&E->mtx);
        if (rc != OK) {
            msg("[rede] jogada recebida e invalida (codigo %d): %s\n", rc, linha);
            enviar_linha(E, "%dE%d", E->meu_id, rc);
        } else {
            msg("%s", tela);
            if (dessinc) msg("[AVISO] checksum/vez diferentes: os jogos dessincronizaram!\n");
        }
        prompt();
        break;
    }
    case 'C': {                                   /* chat */
        char txt[CHAT_MAX + 1];
        copiar_limpo(txt, arg, sizeof txt);
        pthread_mutex_lock(&E->mtx);
        char nome[NOME_MAX];
        snprintf(nome, sizeof nome, "%s", E->nome[opp]);
        pthread_mutex_unlock(&E->mtx);
        msg("\n[chat] %s: %s\n", nome, txt);
        prompt();
        break;
    }
    case 'D': {                                   /* oponente desistiu */
        int ok = 0;
        pthread_mutex_lock(&E->mtx);
        if (E->fase == FASE_JOGANDO) {
            finalizar(E, opp);
            montar_tela(E, tela, sizeof tela);
            ok = 1;
        }
        pthread_cond_broadcast(&E->cv);
        pthread_mutex_unlock(&E->mtx);
        if (ok) msg("%s", tela);
        prompt();
        break;
    }
    case 'R': {                                   /* oponente quer revanche */
        int estado = 0;
        pthread_mutex_lock(&E->mtx);
        if (E->fase == FASE_FIM) {
            E->revanche[opp] = 1;
            estado = 1;
            if (tentar_revanche(E)) { estado = 2; montar_tela(E, tela, sizeof tela); }
        }
        pthread_cond_broadcast(&E->cv);
        pthread_mutex_unlock(&E->mtx);
        if (estado == 2)      msg("\nNova partida!%s", tela);
        else if (estado == 1) msg("\nO oponente quer revanche! Digite /revanche para aceitar.\n");
        prompt();
        break;
    }
    case 'E':
        msg("\n[rede] o oponente rejeitou uma mensagem nossa (erro %s).\n", arg);
        prompt();
        break;
    case 'Q':
        pthread_mutex_lock(&E->mtx);
        E->encerrar = 1;
        pthread_cond_broadcast(&E->cv);
        pthread_mutex_unlock(&E->mtx);
        msg("\nO oponente saiu do jogo. Pressione ENTER para encerrar.\n");
        break;
    default:
        msg("[rede] tipo de mensagem desconhecido '%c' ignorado.\n", tipo);
        break;
    }
}

static void *thread_receptora(void *arg)
{
    Estado *E = arg;
    char acc[LINHA_MAX];          /* acumula bytes ate achar '\n' (TCP e um fluxo!) */
    char tmp[256];
    int len = 0;

    for (;;) {
        int k = (int)recv(E->sock, tmp, sizeof tmp, 0);
        if (k <= 0) break;
        for (int i = 0; i < k; i++) {
            if (tmp[i] == '\n') {
                acc[len] = '\0';
                if (len > 0) processar_msg(E, acc);
                len = 0;
            } else if (tmp[i] != '\r' && len < (int)sizeof acc - 1) {
                acc[len++] = tmp[i];
            }
        }
    }
    /* conexao fechada ou erro */
    pthread_mutex_lock(&E->mtx);
    int ja_sabia = E->encerrar;
    E->encerrar = 1;
    pthread_cond_broadcast(&E->cv);
    pthread_mutex_unlock(&E->mtx);
    if (!ja_sabia) msg("\nConexao com o oponente perdida. Pressione ENTER para encerrar.\n");
    return NULL;
}

/* ---------------------------------------------------------------- */
/*  Conexao                                                          */
/* ---------------------------------------------------------------- */
static sock_t abrir_servidor(int porta)
{
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == SOCK_INVALIDO) { erro_rede("socket"); return SOCK_INVALIDO; }
#ifndef _WIN32
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)porta);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) { erro_rede("bind"); fechar_sock(ls); return SOCK_INVALIDO; }
    if (listen(ls, 1) < 0)                             { erro_rede("listen"); fechar_sock(ls); return SOCK_INVALIDO; }
    msg("Aguardando o oponente na porta %d...\n", porta);
    sock_t s = accept(ls, NULL, NULL);
    if (s == SOCK_INVALIDO) erro_rede("accept");
    fechar_sock(ls);
    return s;
}

static sock_t conectar_servidor(const char *ip, int porta)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALIDO) { erro_rede("socket"); return SOCK_INVALIDO; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(ip);
    a.sin_port = htons((unsigned short)porta);
    if (a.sin_addr.s_addr == INADDR_NONE) { msg("[erro] IP invalido: %s\n", ip); fechar_sock(s); return SOCK_INVALIDO; }
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        erro_rede("connect (o servidor ja esta rodando?)");
        fechar_sock(s);
        return SOCK_INVALIDO;
    }
    return s;
}

/* ---------------------------------------------------------------- */
/*  Laco do jogador humano (teclado)                                 */
/* ---------------------------------------------------------------- */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void loop_humano(Estado *E)
{
    char linha[LINHA_MAX];
    for (;;) {
        prompt();
        if (!fgets(linha, sizeof linha, stdin)) break;      /* EOF */
        pthread_mutex_lock(&E->mtx);
        int fim = E->encerrar;
        pthread_mutex_unlock(&E->mtx);
        if (fim) break;
        trim(linha);
        if (!linha[0]) continue;

        if (linha[0] == '/') {
            if (!strcmp(linha, "/sair"))            break;
            else if (!strcmp(linha, "/ajuda"))      ajuda();
            else if (!strcmp(linha, "/tab"))        mostrar_tabuleiro(E);
            else if (!strcmp(linha, "/hist"))       mostrar_historico(E);
            else if (!strcmp(linha, "/desistir"))   desistir_local(E);
            else if (!strcmp(linha, "/revanche"))   revanche_local(E);
            else if (!strncmp(linha, "/chat ", 6) || !strncmp(linha, "/c ", 3)) {
                char *txt = linha + (linha[2] == ' ' ? 3 : 6);
                char limpo[CHAT_MAX + 1];
                copiar_limpo(limpo, txt, sizeof limpo);
                if (limpo[0] && enviar_linha(E, "%dC%s", E->meu_id, limpo) == 0)
                    msg("[voce]: %s\n", limpo);
            } else msg("Comando desconhecido. Digite /ajuda.\n");
        } else if (strlen(linha) == 2 && isalpha((unsigned char)linha[0]) && isdigit((unsigned char)linha[1])) {
            jogar_local(E, linha[1] - '1', tolower((unsigned char)linha[0]) - 'a');
        } else {
            msg("Nao entendi. Para jogar digite coluna+linha (ex: d3). /ajuda mostra os comandos.\n");
        }
    }
}

/* ---------------------------------------------------------------- */
/*  Bot (opcional, --bot N): joga sozinho com estrategia gulosa.     */
/*  Serve para testar a rede e para demonstrar o jogo com 1 pessoa.  */
/* ---------------------------------------------------------------- */
static int escolher_guloso(Estado *E, int *pl, int *pc)   /* mtx travado */
{
    int melhor = 0;
    for (int l = 0; l < N; l++)
        for (int c = 0; c < N; c++) {
            int v = total_viradas(E->tab, l, c, E->meu_id);
            if (v > melhor) { melhor = v; *pl = l; *pc = c; }
        }
    return melhor > 0;
}

static void loop_bot(Estado *E, int partidas)
{
    int jogos_vistos = 0;
    pthread_mutex_lock(&E->mtx);
    while (!E->encerrar) {
        if (E->fase == FASE_JOGANDO && E->vez == E->meu_id) {
            int l = 0, c = 0;
            int tem = escolher_guloso(E, &l, &c);
            pthread_mutex_unlock(&E->mtx);
            if (tem) jogar_local(E, l, c);
            pthread_mutex_lock(&E->mtx);
        } else if (E->fase == FASE_FIM && !E->revanche[E->meu_id]) {
            if (++jogos_vistos >= partidas) break;         /* acabaram as partidas */
            pthread_mutex_unlock(&E->mtx);
            revanche_local(E);
            pthread_mutex_lock(&E->mtx);
        } else {
            pthread_cond_wait(&E->cv, &E->mtx);            /* dorme ate algo mudar */
        }
    }
    pthread_mutex_unlock(&E->mtx);
}

/* ---------------------------------------------------------------- */
/*  main                                                             */
/* ---------------------------------------------------------------- */
static void ler_texto(const char *pergunta, char *dst, size_t n, const char *padrao)
{
    printf("%s", pergunta);
    fflush(stdout);
    if (!fgets(dst, (int)n, stdin)) dst[0] = '\0';
    trim(dst);
    if (!dst[0]) snprintf(dst, n, "%s", padrao);
}

int main(int argc, char **argv)
{
    int servidor = -1, porta = PORTA_PADRAO, bot = 0;
    char ip[64] = "127.0.0.1", nome[NOME_MAX] = "";
    int pos = 0;                                   /* argumentos posicionais */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nome") && i + 1 < argc)      copiar_limpo(nome, argv[++i], sizeof nome);
        else if (!strcmp(argv[i], "--bot") && i + 1 < argc)  bot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "servidor"))               servidor = 1;
        else if (!strcmp(argv[i], "cliente"))                servidor = 0;
        else if (servidor == 0 && pos == 0 && strchr(argv[i], '.')) { snprintf(ip, sizeof ip, "%s", argv[i]); pos++; }
        else if (atoi(argv[i]) > 0)                          porta = atoi(argv[i]);
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "Falha no WSAStartup\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);                      /* send() devolve erro em vez de matar o processo */
#endif

    if (servidor < 0) {                            /* modo interativo */
        char op[16], tmp[16];
        printf("=== REVERSI EM REDE ===\n1) Criar partida (servidor)\n2) Entrar em partida (cliente)\n");
        ler_texto("Escolha: ", op, sizeof op, "1");
        servidor = (op[0] != '2');
        if (!servidor) ler_texto("IP do servidor [127.0.0.1]: ", ip, sizeof ip, "127.0.0.1");
        ler_texto("Porta [5000]: ", tmp, sizeof tmp, "5000");
        if (atoi(tmp) > 0) porta = atoi(tmp);
    }
    if (!nome[0]) {
        if (bot) snprintf(nome, sizeof nome, "Bot%d", servidor ? 1 : 2);
        else {
            char pad[16];
            snprintf(pad, sizeof pad, "Jogador%d", servidor ? 1 : 2);
            ler_texto("Seu nome: ", nome, sizeof nome, pad);
            char limpo[NOME_MAX];
            copiar_limpo(limpo, nome, sizeof limpo);
            snprintf(nome, sizeof nome, "%s", limpo);
        }
    }
    g_interativo = (bot == 0);

    /* Estado compartilhado: alocado na HEAP e entregue por ponteiro as threads. */
    Estado *E = calloc(1, sizeof *E);
    if (!E) { fprintf(stderr, "Sem memoria\n"); return 1; }
    pthread_mutex_init(&E->mtx, NULL);
    pthread_cond_init(&E->cv, NULL);
    E->meu_id = servidor ? 1 : 2;
    E->vencedor = -1;
    snprintf(E->nome[1], NOME_MAX, "Jogador1");
    snprintf(E->nome[2], NOME_MAX, "Jogador2");
    snprintf(E->nome[E->meu_id], NOME_MAX, "%s", nome);

    E->sock = servidor ? abrir_servidor(porta) : conectar_servidor(ip, porta);
    if (E->sock == SOCK_INVALIDO) {
        free(E);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    msg("Conectado! Voce e o jogador %d (%s, pecas %s).\n", E->meu_id, nome,
        servidor ? "pretas X - comecam" : "brancas O");

    pthread_t th;
    if (pthread_create(&th, NULL, thread_receptora, E) != 0) { msg("[erro] pthread_create\n"); return 1; }

    /* Handshake: troca de nomes. A partida comeca quando os dois lados
     * ja enviaram/receberam o nome (quem completar primeiro, inicia). */
    enviar_linha(E, "%dN%s", E->meu_id, nome);
    pthread_mutex_lock(&E->mtx);
    E->nome_enviado = 1;
    int iniciou = tentar_iniciar(E);
    char tela[TELA_MAX];
    if (iniciou) montar_tela(E, tela, sizeof tela);
    while (E->fase == FASE_LOBBY && !E->encerrar)
        pthread_cond_wait(&E->cv, &E->mtx);          /* espera o oponente */
    pthread_mutex_unlock(&E->mtx);
    if (iniciou) { msg("\nPartida iniciada! Digite /ajuda para ver os comandos.%s", tela); }

    if (bot) loop_bot(E, bot);
    else     loop_humano(E);

    /* Encerramento ordenado */
    pthread_mutex_lock(&E->mtx);
    E->encerrar = 1;
    pthread_mutex_unlock(&E->mtx);
    enviar_linha(E, "%dQ", E->meu_id);
    shutdown(E->sock, DESLIGAR_AMBOS);               /* destrava o recv() da thread receptora */
    pthread_join(th, NULL);
    fechar_sock(E->sock);

    if (E->dessincronizado) msg("[AVISO] houve dessincronizacao durante o jogo.\n");
    msg("Placar final: %s %d x %d %s  (empates: %d)\n", E->nome[1], E->vitorias[1],
        E->vitorias[2], E->nome[2], E->empates);

    free(E->hist);                                   /* libera a memoria dinamica */
    pthread_mutex_destroy(&E->mtx);
    pthread_cond_destroy(&E->cv);
    free(E);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
