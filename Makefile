CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g 

.PHONY: all clean

all: server client

server: src/server.c 
	$(CC) $(CFLAGS) -o $@ $<

client: src/client.c 
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) server client
