CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -lpthread
TARGET = planificador

all: $(TARGET)

$(TARGET): planificador.c
	$(CC) $(CFLAGS) planificador.c -o $(TARGET)

clean:
	rm -f $(TARGET)