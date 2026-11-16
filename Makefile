CC     = gcc
CFLAGS = -Wall -Wextra -O2

reversi: reversi.c
	$(CC) $(CFLAGS) reversi.c -o reversi -lpthread

clean:
	rm -f reversi
