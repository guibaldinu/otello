# Reversi (Othello) em rede — Trabalho 02, Sistemas Operacionais

Jogo de Reversi para 2 jogadores. Cada jogador roda **um processo** e os dois
conversam por **socket TCP** usando um protocolo de mensagens próprio.
Escrito em C com `winsock2` + `pthreads` (e também compila em Linux/macOS).

## Compilar

| Sistema | Comando |
|---|---|
| Windows (MinGW) | `gcc reversi.c -o reversi.exe -lws2_32 -lpthread` (ou rode `compilar.bat`) |
| Linux / macOS | `gcc reversi.c -o reversi -lpthread` (ou `make`) |

## Jogar

Abra **dois terminais**.

```
Terminal 1:  reversi servidor            (jogador 1, peças pretas X, começa)
Terminal 2:  reversi cliente 127.0.0.1   (jogador 2, peças brancas O)
```

- Em computadores diferentes, use o IP do servidor (`ipconfig` no Windows) no lugar de
  `127.0.0.1` e libere a porta (padrão `5000`) no firewall. Porta diferente:
  `reversi servidor 6000` / `reversi cliente 192.168.0.10 6000`.
- Sem argumentos o programa abre um menu perguntando o que fazer.
- Opções: `--nome Fulano` (define o nome) e `--bot N` (o processo joga sozinho N partidas,
  com estratégia gulosa — usado nos testes e útil para demonstração).

### Comandos durante o jogo

| Comando | O que faz |
|---|---|
| `d3` | joga na casa d3 (coluna a–h + linha 1–8) |
| `/chat texto` (`/c`) | manda mensagem ao oponente |
| `/tab` | redesenha o tabuleiro |
| `/hist` | mostra as últimas jogadas |
| `/desistir` | desiste da partida |
| `/revanche` | pede nova partida (quando as duas pessoas pedem, recomeça) |
| `/sair` | encerra o programa |

Casas marcadas com `*` são as jogadas válidas da sua vez.

## Regras implementadas

Reversi oficial: tabuleiro 8x8, pretas começam, a jogada precisa virar ao menos uma peça
do adversário (8 direções). Quem não tem jogada válida **passa a vez automaticamente**; o jogo
termina quando nenhum dos dois pode jogar, e vence quem tiver mais peças (ou há empate).
O placar de vitórias é acumulado entre as revanches.

## Protocolo de mensagens

Cada mensagem é **uma linha de texto** terminada em `\n`:

```
<jogador><TIPO><argumentos>
```

`<jogador>` (`1` ou `2`) identifica quem **enviou**. Mensagens com remetente errado são ignoradas.

| Mensagem | Exemplo | Significado |
|---|---|---|
| `N` nome | `1NFulano` | handshake: troca de nomes ao conectar |
| `J` jogada | `1Jd3;2;9F3A12BC` | jogou em **d3**; após a jogada a vez é do jogador **2** (`0` = fim de jogo); checksum do tabuleiro |
| `C` chat | `2Coi!` | texto de chat |
| `D` desistência | `1D` | quem enviou desistiu |
| `R` revanche | `2R` | quem enviou quer jogar de novo |
| `E` erro | `2E3` | recusei uma mensagem sua (código do erro) |
| `Q` saída | `1Q` | quem enviou está saindo |

Como TCP é um fluxo de bytes (não preserva "fronteiras" de mensagens), a thread receptora
acumula os bytes em um buffer e só processa quando encontra o `\n`.

## Como funciona por dentro

### Memória compartilhada
Cada processo tem **uma struct `Estado`** alocada na heap (`calloc`) e compartilhada pelas duas
threads do processo: a principal (teclado) e a receptora (rede). Ela guarda tabuleiro, vez,
fase, placar, nomes, pedidos de revanche e o histórico de jogadas.

Entre os dois processos (que podem estar em máquinas diferentes) não existe RAM em comum, então
o estado é **replicado**: cada jogada vira uma mensagem e os dois lados aplicam a mesma função
`aplicar_jogada()`. Para provar que as cópias continuam iguais, toda mensagem de jogada leva a
**vez seguinte** e um **checksum** do tabuleiro; se o receptor calcular algo diferente, mostra
`[AVISO] ... dessincronizaram`.

### Exclusão mútua
- `Estado.mtx` (mutex): protege **todo** o estado compartilhado. Toda leitura/escrita do
  tabuleiro, vez, placar etc. acontece com ele travado.
- `Estado.cv` (variável de condição): quem precisa esperar uma mudança de estado dorme em
  `pthread_cond_wait` (esperar o oponente conectar; o bot esperar a vez dele) e é acordado por
  `pthread_cond_broadcast` quando a thread receptora ou a principal alteram o estado.
- `mtx_envio`: serializa o `send()`, para duas threads nunca misturarem bytes de mensagens
  diferentes no mesmo socket.
- `mtx_tela`: serializa o `printf()`, para o tabuleiro desenhado por uma thread não se misturar
  com o chat escrito pela outra. O tabuleiro é montado em um buffer local (dentro do `mtx`) e
  impresso depois, com o `mtx` já liberado.
- **Sem deadlock:** `mtx_envio` e `mtx_tela` nunca são travados dentro de outro mutex nem um
  dentro do outro; o `mtx` do estado nunca fica travado durante `send()`/`recv()`/`printf()`.
- **Exclusão lógica (a variável `vez`):** só quem tem a vez pode alterar o tabuleiro.
  `aplicar_jogada()` rejeita jogadas fora de turno, tanto as locais quanto as recebidas.

### Gerenciamento de memória
- Histórico de jogadas = **vetor dinâmico** (`realloc`, capacidade dobra: 16, 32, 64…), reaproveitado
  entre revanches e liberado com `free` ao final.
- `Estado` alocado com `calloc` e liberado no encerramento, junto com mutex/cond e socket.
- Encerramento ordenado: envia `Q`, faz `shutdown()` do socket (destrava o `recv()` da outra
  thread), `pthread_join`, e só então libera a memória.

## Testes realizados

Executados em Linux (gcc 13):
- partidas completas entre dois processos, com 3 partidas seguidas usando revanche;
- compilação com `-Wall -Wextra` sem avisos;
- AddressSanitizer + UBSan (sem erros nem vazamentos) e ThreadSanitizer (sem corrida de dados);
- as duas pontas terminaram sempre com o mesmo placar e **0** avisos de dessincronização;
- caminho humano: jogada inválida, comando inválido, chat, histórico, desistência, revanche, saída.

> A parte específica de Windows (`winsock2`, `WSAStartup`, `closesocket`) está no código sob
> `#ifdef _WIN32`, mas foi escrita sem compilar em Windows. Compile e teste no seu computador.

## Estrutura do código (`reversi.c`)

1. Constantes e tipos (`Estado`, `Jogada`)
2. Saída em tela atômica (`msg`)
3. Regras do Reversi (funções puras)
4. Operações sobre o `Estado` (`nova_partida`, `aplicar_jogada`, `finalizar`)
5. Desenho do tabuleiro
6. Envio pela rede
7. Ações do jogador (`jogar_local`, `desistir_local`, `revanche_local`)
8. Thread receptora e processamento das mensagens
9. Conexão (servidor/cliente)
10. Laço do humano, laço do bot e `main`
