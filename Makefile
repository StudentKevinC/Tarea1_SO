CC = gcc
CFLAGS = -Wall -Wextra -std=c17
PROGRAMA = planificador

all:
	$(CC) $(CFLAGS) planificador.c -o $(PROGRAMA) -lpthread

clean:
	rm -f $(PROGRAMA)