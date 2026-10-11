#ifndef COMM_H
#define COMM_H


#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>




/* Creates a socket and sockaddr struct 
 * 
 * Returns:
 * - int server_sockfd: file descriptor for established socket
 * - int -1: error occured establishing socket
 *
 */
int comm_socket_setup();




/* Initializes the sockaddr_un struct argument to the path of the server socket
 *
 * Parameters:
 * - struct sockaddr_un *addr: sockaddr_un struct pointer to be initialized
 *
 * Returns:
 * - struct sockaddr_un *addr: initialized sockaddr_un struct
 *
 */
struct sockaddr_un *comm_addr_setup(struct sockaddr_un *addr);




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
int comm_server_setup(int server_sockfd, struct sockaddr_un *addr, int backlog);




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
int comm_server_accept(int listen_sockfd);




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
int comm_client_connect(int server_sockfd, struct sockaddr_un *addr);




/* Closes the communication socket specified in the fd argument*/
void comm_close(int fd);


#endif
