@echo off
gcc -Wall -Wextra -O2 reversi.c -o reversi.exe -lws2_32 -lpthread
if %errorlevel% neq 0 (
  echo Erro na compilacao. Confira se o MinGW esta instalado e no PATH.
) else (
  echo Compilado: reversi.exe
  echo Terminal 1: reversi servidor
  echo Terminal 2: reversi cliente 127.0.0.1
)
pause
