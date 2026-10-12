#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comm.h"

/* Display untrusted server text without passing terminal control bytes through */
static void display_message(const char *message)
{
    for (const unsigned char *p = (const unsigned char *)message; *p; p++) {
        putchar(*p >= 32 && *p <= 126 ? *p : '?');
    }
    putchar('\n');
    fflush(stdout);
}

/* Convert terminal commands to framed requests for the moderator */
static void send_command(comm_peer_t *peer, const char *line)
{
    const char *text = NULL;
    const char *verb = NULL;
    if (strncmp(line, "/chat ", 6) == 0) {
        verb = "CHAT";
        text = line + 6;
    } else if (strncmp(line, "/vote ", 6) == 0 ||
               strncmp(line, "/eat ", 5) == 0) {
        verb = "VOTE";
        text = line + (line[1] == 'e' ? 5 : 6);
    } else {
        puts("Use /chat message or /vote UID");
        return;
    }
    char payload[COMM_MAX_PAYLOAD + 1];
    int written = snprintf(payload, sizeof(payload), "%s %s", verb, text);
    if (written < 0 || (size_t)written >= sizeof(payload) ||
        comm_peer_queue(peer, payload) == -1) {
        puts("Could not queue command");
    }
}

int main(void)
{
    struct sockaddr_un addr;
    comm_addr_setup(&addr);

    int client_sockfd = comm_socket_setup();
    if (client_sockfd == -1) {
        perror("client: create socket");
        return EXIT_FAILURE;
    }

    int result = EXIT_SUCCESS;
    if (comm_client_connect(client_sockfd, &addr) == -1) {
        perror("client: connect");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    comm_peer_t peer;
    comm_peer_init(&peer, client_sockfd);
    puts("Connected; use /chat message during wolf chat, then /vote UID to eat");
    fflush(stdout);
    bool stdin_open = true;
    bool connected = true;
    while (connected) {
        comm_watch_t watches[2] = {
            {.fd = client_sockfd, .want_write = comm_peer_has_output(&peer)},
            {.fd = stdin_open ? 0 : -1}
        };
        if (comm_wait(watches, 2, -1) == -1) {
            perror("client: wait");
            result = EXIT_FAILURE;
            break;
        }
        if (watches[0].writable && comm_peer_flush(&peer) == -1) {
            perror("client: send");
            result = EXIT_FAILURE;
            break;
        }
        if (watches[0].readable) {
            for (int frame = 0; frame < 8; frame++) {
                char payload[COMM_MAX_PAYLOAD + 1];
                int status = comm_peer_read(&peer, payload, sizeof(payload));
                if (status == 0) {
                    break;
                }
                if (status == 2) {
                    puts("Server closed the connection");
                    connected = false;
                    break;
                }
                if (status == -1) {
                    perror("client: receive");
                    result = EXIT_FAILURE;
                    connected = false;
                    break;
                }
                display_message(payload);
            }
        }
        if (connected && stdin_open && watches[1].readable) {
            char line[256];
            if (fgets(line, sizeof(line), stdin) == NULL) {
                stdin_open = false;
            } else {
                size_t length = strlen(line);
                if (length > 0 && line[length - 1] == '\n') {
                    line[length - 1] = '\0';
                }
                send_command(&peer, line);
            }
        }
    }

cleanup:
    if (comm_close(client_sockfd) == -1) {
        perror("client: close connection");
        result = EXIT_FAILURE;
    }
    return result;
}
