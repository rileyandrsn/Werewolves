#include <stdio.h>
#include <stdlib.h>

#include "comm.h"

int main(void)
{
    struct sockaddr_un addr;
    comm_addr_setup(&addr);

    int server_sockfd = comm_socket_setup();
    if (server_sockfd == -1) {
        perror("server: create socket");
        return EXIT_FAILURE;
    }

    if (comm_server_setup(server_sockfd, &addr, 32) == -1) {
        perror("server: bind/listen");
        if (comm_close(server_sockfd) == -1) {
            perror("server: close listener");
        }
        return EXIT_FAILURE;
    }

    printf("Waiting for players to connect at %s...\n", addr.sun_path);
    fflush(stdout);

    int result = EXIT_SUCCESS;
    int client_fd = comm_server_accept(server_sockfd);
    if (client_fd == -1) {
        perror("server: accept");
        result = EXIT_FAILURE;
    } else {
        puts("Player connected!");
        if (comm_close(client_fd) == -1) {
            perror("server: close player connection");
            result = EXIT_FAILURE;
        }
    }

    if (comm_close(server_sockfd) == -1) {
        perror("server: close listener");
        result = EXIT_FAILURE;
    }
    if (comm_server_cleanup(&addr) == -1) {
        perror("server: remove socket path");
        result = EXIT_FAILURE;
    }

    return result;
}
