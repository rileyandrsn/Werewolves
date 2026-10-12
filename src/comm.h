#ifndef COMM_H
#define COMM_H

#include <stdbool.h>
#include <stdint.h>
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
    bool want_write; // Also watch for space in the send buffer
    bool readable; // Input, EOF, or socket error is ready
    bool writable; // Output may be sent without blocking
} comm_watch_t;

#define COMM_MAX_PAYLOAD 512
#define COMM_OUTPUT_CAPACITY 8192

typedef struct {
    int fd;
    unsigned char input[4 + COMM_MAX_PAYLOAD];
    size_t input_used;
    size_t input_needed;
    unsigned char output[COMM_OUTPUT_CAPACITY];
    size_t output_used;
    size_t output_sent;
} comm_peer_t;

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
void comm_peer_init(comm_peer_t *peer, int fd);

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
int comm_peer_read(comm_peer_t *peer, char *payload, size_t capacity);

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
int comm_peer_queue(comm_peer_t *peer, const char *payload);

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
                          const char *payload, int results[]);

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
int comm_peer_flush(comm_peer_t *peer);

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
bool comm_peer_has_output(const comm_peer_t *peer);

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
