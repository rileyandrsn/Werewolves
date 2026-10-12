#include <stdio.h>
#include <stdlib.h>

#include "comm.h"


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
    } else {
        puts("Connected to moderator; waiting for the server to close the connection");
        fflush(stdout);

        comm_watch_t watch = {.fd = client_sockfd};
        while (!watch.ready) {
            if (comm_wait(&watch, // Server connection
                          1,      // One socket
                          -1) == -1) { // Wait without a timeout
                perror("client: wait for server");
                result = EXIT_FAILURE;
                break;
            }
        }
        if (result == EXIT_SUCCESS) {
            puts("Server closed the connection");
        }
    }

    if (comm_close(client_sockfd) == -1) {
        perror("client: close connection");
        result = EXIT_FAILURE;
    }

    return result;
}
