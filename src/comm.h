#ifndef COMM_H
#define COMM_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/un.h>

/*
 * UNIX stream connection helpers
 * Callers report errors and close their sockets
 */

typedef struct {
    int fd; // Socket to watch; negative values are ignored
    bool ready; // Socket has input, a disconnect, or an error
} comm_watch_t;

/*
 * Wait for socket activity without reading any data
 * Interrupted waits return so the caller can update its deadline
 *
 * Parameters:
 * - comm_watch_t *watches: Socket list with ready flags filled on return
 * - size_t count: Number of entries in watches
 * - int timeout_ms: Maximum wait in milliseconds; -1 waits indefinitely
 *
 * Returns:
 * - int count (> 0): Number of ready sockets
 * - int 0: Timeout or signal interruption
 * - int -1: Wait failed; errno gives the error
 */
int comm_wait(comm_watch_t *watches, size_t count, int timeout_ms);

/*
 * Create a blocking UNIX stream socket
 *
 * Parameters:
 * - void: No parameters
 *
 * Returns:
 * - int sockfd (>= 0): New socket on success; caller must close it
 * - int -1: Creation failed; errno gives the error
 */
int comm_socket_setup(void);

/*
 * Set the server socket address
 *
 * Parameters:
 * - struct sockaddr_un *addr: Non-NULL address storage to initialize
 *
 * Returns:
 * - struct sockaddr_un *addr: The initialized address pointer; no error return
 */
struct sockaddr_un *comm_addr_setup(struct sockaddr_un *addr);

/*
 * Make the server socket nonblocking, bind it, and listen for connections
 * Keep existing paths; try to remove the new path if listen fails
 *
 * Parameters:
 * - int server_sockfd: Unbound server socket; caller must close it
 * - const struct sockaddr_un *addr: Non-NULL server address to bind
 * - int backlog: Pending connection limit requested from the OS
 *
 * Returns:
 * - int 0: Socket is bound and listening
 * - int -1: Setup failed; errno keeps that error after cleanup
 */
int comm_server_setup(int server_sockfd, const struct sockaddr_un *addr, int backlog);

/*
 * Accept one client connection; retry if interrupted by a signal
 * The listener created by comm_server_setup is nonblocking
 *
 * Parameters:
 * - int listen_sockfd: Listening server socket; stays open
 *
 * Returns:
 * - int client_fd (>= 0): Accepted connection; caller must close it
 * - int -1: Accept failed; EAGAIN or EWOULDBLOCK means no client is waiting
 */
int comm_server_accept(int listen_sockfd);

/*
 * Read the peer's Linux user ID from the kernel
 *
 * Parameters:
 * - int fd: Connected UNIX socket
 * - uid_t *uid: Non-NULL pointer to store the peer's user ID
 *
 * Returns:
 * - int 0: User ID stored in uid
 * - int -1: Lookup failed; errno gives the error and uid stays unchanged
 */
int comm_peer_uid(int fd, uid_t *uid);

/*
 * Connect a client socket to the server
 *
 * Parameters:
 * - int client_sockfd: Unconnected client socket; caller must close it
 * - const struct sockaddr_un *addr: Non-NULL server address to connect to
 *
 * Returns:
 * - int 0: Connected to the server
 * - int -1: Connection failed; errno gives the error
 */
int comm_client_connect(int client_sockfd, const struct sockaddr_un *addr);

/*
 * Close a socket without removing its path
 * Do not retry on Linux; the descriptor may already be reused
 *
 * Parameters:
 * - int fd: Socket descriptor to close
 *
 * Returns:
 * - int 0: Socket closed
 * - int -1: Close reported an error; errno gives the error
 */
int comm_close(int fd);

/*
 * Remove the server socket path without closing sockets
 * Only use for this server's bound path in a trusted directory
 * Path ownership is not checked
 *
 * Parameters:
 * - const struct sockaddr_un *addr: Non-NULL address with the path to remove
 *
 * Returns:
 * - int 0: Path removed
 * - int -1: Removal failed, including a missing path; errno gives the error
 */
int comm_server_cleanup(const struct sockaddr_un *addr);

#endif
