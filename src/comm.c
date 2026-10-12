#define _GNU_SOURCE // Enable Linux peer credentials

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "comm.h"


#define SERVER_SOCK_PATH "/tmp/ww_server.sock"

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
int comm_wait(comm_watch_t *watches, size_t count, int timeout_ms)
{
    if (watches == NULL || count == 0 || count > INT_MAX || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd *fds = calloc(count,         // Number of sockets
                                sizeof(*fds)); // Storage per socket
    if (fds == NULL) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        watches[i].ready = false;
        fds[i].fd = watches[i].fd;
        fds[i].events = POLLIN;
    }

    int result = poll(fds,         // Sockets to watch
                      count,       // Number of sockets
                      timeout_ms); // Maximum wait
    int saved_errno = errno;
    if (result > 0) {
        for (size_t i = 0; i < count; i++) {
            watches[i].ready = fds[i].revents != 0;
        }
    }
    free(fds); // Temporary poll storage

    if (result == -1 && saved_errno == EINTR) {
        return 0;
    }
    errno = saved_errno;
    return result;
}

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
int comm_socket_setup(void)
{
    return socket(AF_UNIX,     // UNIX domain address family
                  SOCK_STREAM, // Ordered byte stream
                  0);          // Default protocol
}

/*
 * Set the server socket address
 *
 * Parameters:
 * - struct sockaddr_un *addr: Non-NULL address storage to initialize
 *
 * Returns:
 * - struct sockaddr_un *addr: The initialized address pointer; no error return
 */
struct sockaddr_un *comm_addr_setup(struct sockaddr_un *addr)
{
    memset(addr,           // Address of sockaddr_un struct to clear
           0,              // Fill with zero
           sizeof(*addr)); // Address size (bytes)
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path,            // Path destination
           SERVER_SOCK_PATH,          // Server socket path
           sizeof(SERVER_SOCK_PATH)); // Path size including the NULL terminator
    return addr;
}

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
int comm_server_setup(int server_sockfd, const struct sockaddr_un *addr, int backlog)
{
    int flags = fcntl(server_sockfd, // Server socket
                      F_GETFL);      // Read current flags
    if (flags == -1 || fcntl(server_sockfd,         // Server socket
                             F_SETFL,              // Update socket flags
                             flags | O_NONBLOCK) == -1) { // Avoid blocking in accept
        return -1;
    }

    if (bind(server_sockfd,                 // Server socket
             (const struct sockaddr *)addr, // Server address as a generic socket address
             sizeof(*addr)) == -1) {        // Address size (bytes)
        return -1;
    }

    if (listen(server_sockfd,    // Bound server socket
               backlog) == -1) { // Pending connection limit
        int saved_errno = errno;

        /* Closing a socket does not remove its path */
        comm_server_cleanup(addr); // Address with the path to remove
        errno = saved_errno;
        return -1;
    }

    return 0;
}

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
int comm_server_accept(int listen_sockfd)
{
    int client_fd;

    /* Retry if a signal interrupts accept */
    do {
        client_fd = accept(listen_sockfd, // Listening socket
                           NULL,          
                           NULL);         
    } while (client_fd == -1 && errno == EINTR);

    return client_fd;
}

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
int comm_peer_uid(int fd, uid_t *uid)
{
    if (uid == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct ucred credentials = {0};
    socklen_t length = sizeof(credentials);

    if (getsockopt(fd,               // Connected socket
                   SOL_SOCKET,      // Socket-level option
                   SO_PEERCRED,     // Kernel-provided peer identity
                   &credentials,    // Credentials output
                   &length) == -1) { // Buffer size in bytes
        return -1;
    }

    *uid = credentials.uid;
    return 0;
}

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
int comm_client_connect(int client_sockfd, const struct sockaddr_un *addr)
{
    return connect(client_sockfd,                 // Client socket
                   (const struct sockaddr *)addr, // Server address as a generic socket address
                   sizeof(*addr));                // Address size (bytes)
}

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
int comm_close(int fd)
{
    return close(fd); // Socket to close
}

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
int comm_server_cleanup(const struct sockaddr_un *addr)
{
    return unlink(addr->sun_path); // Socket path to remove
}
