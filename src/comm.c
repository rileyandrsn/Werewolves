#define _GNU_SOURCE // Enable Linux peer credentials

#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "comm.h"


#define SERVER_SOCK_DIR "/tmp/ww_server"
#define SERVER_SOCK_PATH SERVER_SOCK_DIR "/socket"

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
 * - int ready_count (> 0): Number of ready sockets
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
        watches[i].readable = false;
        watches[i].writable = false;
        fds[i].fd = watches[i].fd;
        fds[i].events = POLLIN | (watches[i].want_write ? POLLOUT : 0);
    }

    int result = poll(fds,         // Sockets to watch
                      count,       // Number of sockets
                      timeout_ms); // Maximum wait
    int saved_errno = errno;
    if (result > 0) {
        for (size_t i = 0; i < count; i++) {
            watches[i].ready = fds[i].revents != 0;
            watches[i].readable = (fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
            watches[i].writable = (fds[i].revents & POLLOUT) != 0;
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
 * Set up frame buffers for a connected socket
 *
 * Parameters:
 * - comm_peer_t *peer: Non-NULL caller-owned transport state to initialize
 * - int fd: Connected socket descriptor owned by the caller
 *
 * Returns:
 * - void: No return value
 */
void comm_peer_init(comm_peer_t *peer, int fd)
{
    memset(peer, 0, sizeof(*peer));
    peer->fd = fd;
}

/*
 * Read at most one length-prefixed text frame without blocking
 * A frame is a 4-byte network-order length followed by 1 to 512 payload bytes
 * The payload is ASCII command or notice text and cannot contain NUL
 *
 * Parameters:
 * - comm_peer_t *peer: Non-NULL connection state and socket
 * - char *payload: Non-NULL destination for text and a trailing NUL
 * - size_t capacity: Destination size including room for the trailing NUL
 *
 * Returns:
 * - int 1: Complete frame copied to payload
 * - int 0: More bytes are needed; call again when readable
 * - int 2: Clean EOF between frames
 * - int -1: Socket, truncated frame, or invalid frame error; errno gives the cause
 */
int comm_peer_read(comm_peer_t *peer, char *payload, size_t capacity)
{
    if (peer == NULL || payload == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        size_t needed = peer->input_needed ? peer->input_needed : 4;
        if (peer->input_used == needed && peer->input_needed == 0) {
            uint32_t network_length;
            memcpy(&network_length, peer->input, sizeof(network_length));
            uint32_t length = ntohl(network_length);
            if (length == 0 || length > COMM_MAX_PAYLOAD || length >= capacity) {
                errno = EMSGSIZE;
                return -1;
            }
            peer->input_needed = 4 + length;
            needed = peer->input_needed;
        }
        if (peer->input_used == needed) {
            size_t length = needed - 4;
            if (memchr(peer->input + 4, '\0', length) != NULL) {
                errno = EPROTO;
                return -1;
            }
            memcpy(payload, peer->input + 4, length);
            payload[length] = '\0';
            peer->input_used = 0;
            peer->input_needed = 0;
            return 1;
        }

        ssize_t received = recv(peer->fd, // Connected socket
                                peer->input + peer->input_used, // Next frame byte
                                needed - peer->input_used, // Bytes needed for this step
                                MSG_DONTWAIT); // Avoid blocking on partial frames
        if (received > 0) {
            peer->input_used += (size_t)received;
            continue;
        }
        if (received == 0) {
            if (peer->input_used != 0) {
                errno = EPROTO;
                return -1;
            }
            return 2;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
}

/*
 * Queue one length-prefixed text frame for nonblocking delivery
 * Queue size is bounded; success does not mean the game accepted a command
 *
 * Parameters:
 * - comm_peer_t *peer: Non-NULL connection state to queue on
 * - const char *payload: NUL-terminated text of 1 to 512 bytes
 *
 * Returns:
 * - int 0: Frame queued for later delivery
 * - int -1: Invalid input or full queue; errno gives the cause
 */
int comm_peer_queue(comm_peer_t *peer, const char *payload)
{
    if (peer == NULL || payload == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t length = strlen(payload);
    if (length == 0 || length > COMM_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    if (peer->output_sent != 0) {
        size_t pending = peer->output_used - peer->output_sent;
        memmove(peer->output, peer->output + peer->output_sent, pending);
        peer->output_used = pending;
        peer->output_sent = 0;
    }
    if (length + 4 > COMM_OUTPUT_CAPACITY - peer->output_used) {
        errno = ENOBUFS;
        return -1;
    }
    uint32_t network_length = htonl((uint32_t)length);
    memcpy(peer->output + peer->output_used, &network_length, 4);
    memcpy(peer->output + peer->output_used + 4, payload, length);
    peer->output_used += length + 4;
    return 0;
}

/*
 * Queue one frame for each supplied recipient
 * Group delivery is not atomic; check each result
 *
 * Parameters:
 * - comm_peer_t *peers[]: Non-NULL array of non-NULL recipient states
 * - size_t count: Number of recipients
 * - const char *payload: NUL-terminated text to queue for each recipient
 * - int results[]: Non-NULL array receiving 0 or -1 per recipient
 *
 * Returns:
 * - int success_count (>= 0): Number of recipients queued successfully
 * - int -1: Invalid arrays; errno gives the cause
 */
int comm_peer_queue_group(comm_peer_t *peers[], size_t count,
                          const char *payload, int results[])
{
    if (peers == NULL || results == NULL) {
        errno = EINVAL;
        return -1;
    }
    int success = 0;
    for (size_t i = 0; i < count; i++) {
        results[i] = comm_peer_queue(peers[i], payload);
        if (results[i] == 0) {
            success++;
        }
    }
    return success;
}

/*
 * Send as much queued data as the socket currently accepts
 * Uses MSG_NOSIGNAL so a disconnected peer cannot raise SIGPIPE
 *
 * Parameters:
 * - comm_peer_t *peer: Non-NULL connection state with queued frames
 *
 * Returns:
 * - int 0: Queue drained or socket would block; check has_output
 * - int -1: Send failed; errno gives the cause
 */
int comm_peer_flush(comm_peer_t *peer)
{
    while (peer->output_sent < peer->output_used) {
        ssize_t sent = send(peer->fd, // Connected socket
                            peer->output + peer->output_sent, // Pending bytes
                            peer->output_used - peer->output_sent, // Pending byte count
                            MSG_DONTWAIT | MSG_NOSIGNAL); // Avoid blocking and SIGPIPE
        if (sent > 0) {
            peer->output_sent += (size_t)sent;
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (sent == 0) {
            errno = EPIPE;
        }
        return -1;
    }
    peer->output_used = 0;
    peer->output_sent = 0;
    return 0;
}

/*
 * Check whether frames are still waiting to be sent
 *
 * Parameters:
 * - const comm_peer_t *peer: Non-NULL connection state to inspect
 *
 * Returns:
 * - bool true: Queued bytes remain
 * - bool false: Output queue is empty
 */
bool comm_peer_has_output(const comm_peer_t *peer)
{
    return peer->output_sent < peer->output_used;
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
 * Create a moderator-owned socket directory, then bind and listen
 * The directory is traversable by the group and the socket is group-writable
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
    if (mkdir(SERVER_SOCK_DIR, // Moderator-owned directory
              0750) == -1 && errno != EEXIST) { // Group may traverse but not write
        return -1;
    }
    struct stat directory;
    if (lstat(SERVER_SOCK_DIR, &directory) == -1) {
        return -1;
    }
    if (!S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        errno = EACCES;
        return -1;
    }
    if (chmod(SERVER_SOCK_DIR, // Directory owned by the moderator
              0750) == -1) {    // Group can reach the socket, not replace it
        return -1;
    }

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

    if (chmod(addr->sun_path, // Newly bound socket path
              0660) == -1) {   // Owner and group may connect
        int saved_errno = errno;
        comm_server_cleanup(addr);
        errno = saved_errno;
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
