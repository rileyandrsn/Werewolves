CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g 

.PHONY: all clean

all: server client

server: src/server.c src/comm.c src/comm.h
	$(CC) $(CFLAGS) -o $@ src/server.c src/comm.c

client: src/client.c src/comm.c src/comm.h
	$(CC) $(CFLAGS) -o $@ src/client.c src/comm.c

clean:
	$(RM) server client
