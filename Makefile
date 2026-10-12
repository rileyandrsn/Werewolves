CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g 
num_werewolves ?= 1

# Rebuild the server so changes to num_werewolves always take effect
.PHONY: all clean server

all: server client

server: src/server.c src/comm.c src/comm.h src/game.h
	$(CC) $(CFLAGS) -DNUM_WEREWOLVES=$(num_werewolves) -o $@ src/server.c src/comm.c

client: src/client.c src/comm.c src/comm.h
	$(CC) $(CFLAGS) -o $@ src/client.c src/comm.c

clean:
	$(RM) server client
	@socket_path=/tmp/ww_server.sock; \
	if [ -S "$$socket_path" ] && [ ! -L "$$socket_path" ] && [ -O "$$socket_path" ]; then \
		if [ -r /proc/net/unix ] && \
		   awk -v path="$$socket_path" '$$8 == path { found = 1 } END { exit found }' /proc/net/unix; then \
			$(RM) -- "$$socket_path"; \
		else \
			echo "Keeping $$socket_path: active socket or unable to check"; \
		fi; \
	fi
