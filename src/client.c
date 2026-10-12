#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comm.h"

/* Display untrusted server text without passing terminal control bytes through */
static void display_message(const char *message)
{
    bool phase_boundary = strncmp(message, "WEREWOLF CHAT ", 14) == 0 ||
                          strncmp(message, "WEREWOLF VOTE ", 14) == 0 ||
                          strncmp(message, "WITCH ", 6) == 0 ||
                          strncmp(message, "DAY DISCUSSION ", 15) == 0 ||
                          strncmp(message, "EXILE VOTE ", 11) == 0 ||
                          strncmp(message, "Entering the night", 18) == 0 ||
                          strncmp(message, "Entering the day", 16) == 0 ||
                          strncmp(message, "DAYBREAK", 8) == 0 ||
                          strcmp(message, "********SLEEPING********") == 0 ||
                          strcmp(message, "You have woken up") == 0 ||
                          strstr(message, "********* GAME OVER -") == message;
    if (phase_boundary) {
        putchar('\n');
    }
    for (const unsigned char *p = (const unsigned char *)message; *p; p++) {
        putchar(*p >= 32 && *p <= 126 ? *p : '?');
    }
    putchar('\n');
    if (phase_boundary) {
        putchar('\n');
    }
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
    } else if (strncmp(line, "/say ", 5) == 0) {
        verb = "SAY";
        text = line + 5;
    } else if (strncmp(line, "/vote ", 6) == 0 ||
               strncmp(line, "/eat ", 5) == 0) {
        verb = "VOTE";
        text = line + (line[1] == 'e' ? 5 : 6);
    } else if (strcmp(line, "/heal") == 0) {
        verb = "HEAL";
        text = NULL;
    } else if (strncmp(line, "/poison ", 8) == 0) {
        verb = "POISON";
        text = line + 8;
    } else if (strncmp(line, "/exile ", 7) == 0) {
        verb = "EXILE";
        text = line + 7;
    } else {
        puts("Use /chat message, /vote name, /heal, /poison name, /say message, or /exile name");
        return;
    }
    char payload[COMM_MAX_PAYLOAD + 1];
    int written = text == NULL ? snprintf(payload, sizeof(payload), "%s", verb) :
                   snprintf(payload, sizeof(payload), "%s %s", verb, text);
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
    puts("CONNECTED - waiting for game to start...");
    fflush(stdout);
    bool stdin_open = true;
    bool connected = true;
    char input_line[256];
    size_t input_used = 0;
    bool input_overlong = false;
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
            char chunk[256];
            ssize_t received = read(0,             // Terminal input
                                    chunk,         // Newly available bytes
                                    sizeof(chunk)); // Maximum bytes to read
            if (received == 0) {
                stdin_open = false;
                if (input_used > 0 && !input_overlong) {
                    input_line[input_used] = '\0';
                    send_command(&peer, input_line);
                }
            } else if (received == -1) {
                if (errno != EINTR && errno != EAGAIN) {
                    perror("client: read terminal");
                    stdin_open = false;
                    result = EXIT_FAILURE;
                }
            } else {
                for (ssize_t i = 0; i < received; i++) {
                    if (chunk[i] == '\n') {
                        if (input_overlong) {
                            puts("Input line too long");
                        } else {
                            input_line[input_used] = '\0';
                            send_command(&peer, input_line);
                        }
                        input_used = 0;
                        input_overlong = false;
                    } else if (input_used < sizeof(input_line) - 1) {
                        input_line[input_used++] = chunk[i];
                    } else {
                        input_overlong = true;
                    }
                }
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
