#include "comm.h"
#include <sys/un.h>

int main()
{


int server_sockfd = comm_socket_setup();
if(server_sockfd == -1)
{
    fprintf(stderr,"ERROR | Terminating server | Issue establishing server socket");
    return -1;
}

struct sockaddr_un *addr = {0};
addr = comm_addr_setup(addr);

int setup_result = comm_server_setup(server_sockfd, addr, 32);
if(setup_result != 0)
{
    fprintf(stderr,"ERROR | Terminating server | Issue binding and listening on server socket");
    return -1;
}

int client_fd = comm_server_accept(server_sockfd);

comm_close(client_fd);
comm_close(server_sockfd);


}
