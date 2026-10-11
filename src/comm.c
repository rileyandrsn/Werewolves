#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SERVER_SOCK_PATH "/tmp/ww_server.sock" // Werewolves server UNIX domain socket filepath


/* Creates a socket and sockaddr struct 
 * 
 * Returns:
 *     - int server_sockfd: file descriptor for established socket
 *     - int -1: error occured establishing socket
 *
 */
int comm_socket_setup()
{
    // Create socket endpoint for the server/moderator process
    // * returns file descriptor for new socket upon success
    int server_sockfd = socket(AF_UNIX, // UNIX domain socket
			       SOCK_STREAM, // TCP
			       0); 
    if(server_sockfd == -1)
    { // * returns -1 upon error
   	fprintf(stderr,"ERROR | Server socket unsuccessfully established with return value: %d",server_sockfd);
        return -1;
    }
    return server_sockfd;
}


/* Initializes the sockaddr_un struct argument to the path of the server socket
 *
 * Parameters:
 * - struct sockaddr_un *addr: sockaddr_un struct pointer to be initialized
 *
 * Returns:
 * - struct sockaddr_un *addr: initialized sockaddr_un struct
 *
 */
struct sockaddr_un *comm_addr_setup(struct sockaddr_un *addr)
{
    addr->sun_family = AF_UNIX; // UNIX domain socket address family (AF)
    memcpy(addr->sun_path, // socket pathname
	   SERVER_SOCK_PATH, // path of server socket
	   sizeof(SERVER_SOCK_PATH)); // size of server socket pathname

    return addr; 
}


/* Binds the server's socket to the sockaddr_un struct and begins listening for up to 'backlog' number of connections
 *
 * Parameters:
 * - int server_sockfd: server socket file descriptor
 * - struct sockaddr_un *addr: pointer to sockaddr_un struct
 * - int backlog: number of connections to listen for
 *
 * Returns:
 * - int 0: No errors occured when binding socket to addr struct, or listening on server socket
 * - int -1: Error occured binding to socket or listening on server socket
 *
 */
int comm_server_setup(int server_sockfd, struct sockaddr_un *addr, int backlog)
{
    if(bind(server_sockfd, // server socket file descriptor
	    (struct sockaddr *) addr, // socket address struct
	    sizeof(*addr)) == -1) // size of server address struct
    {
	fprintf(stderr,"ERROR | Address unsuccessfully bound to socket");
	return -1;
    }

    if(listen(server_sockfd, // listen on server socket
	      backlog) == -1) // listen up to 'backlog' pending connections
      {
	fprintf(stderr,"ERROR | Error occured listening on server socket");
	return -1;
      }
    return 0;

}


/* Waits to accept a connection from client(s)
 *
 * Parameters:
 * - int listen_sockfd: socket file descriptor to accept connections on
 *
 * Returns:
 * - int client_fd: file descriptor of accepted client connection
 * - int -1: Error accepting incoming client connection
 *
 */
int comm_server_accept(int listen_sockfd)
{
     int client_fd = accept(listen_sockfd,NULL,NULL);
     if(client_fd == -1)
     {
	fprintf(stderr,"Failed to connect player");
	return -1;
     }

     puts("Player connected!");
     return client_fd;
}


/* Connects a client to the server process's socket
 *
 * Parameters:
 * - int server_sockfd: file descriptor of the server's socket to connect to
 * - struct sockaddr_un *addr: sockaddr_un struct specifying server socket
 *
 * Returns:
 * - int 0: Successfully connected to server
 * - int -1: Error connecting to server
 *
 */
int comm_client_connect(int server_sockfd, struct sockaddr_un *addr)
{
    if(connect(server_sockfd,
	       (struct sockaddr *) addr,
	       sizeof(*addr)) == -1)
    {
	fprintf(stderr, "ERROR | Client failed to connect to server");
	return -1;
    }
    return 0;
}


/* Closes the communication socket specified in the fd argument*/
void comm_close(int fd)
{
    close(fd);
    printf("Closed socket at file descriptor: %d\n", fd);
}

