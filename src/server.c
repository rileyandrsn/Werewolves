#define _POSIX_C_SOURCE 200809L // Enable the monotonic clock

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>

#include "comm.h"
#include "game.h"

#ifndef LOBBY_WAITING_TIME
#define LOBBY_WAITING_TIME 120
#endif

#ifndef MAX_PLAYERS
#define MAX_PLAYERS 16
#endif

#ifndef NUM_WEREWOLVES
#define NUM_WEREWOLVES 1
#endif

_Static_assert(LOBBY_WAITING_TIME >= 0, "Lobby duration must not be negative");
_Static_assert(MAX_PLAYERS > 0, "Player limit must be positive");
_Static_assert(NUM_WEREWOLVES > 0, "There must be at least one werewolf");
_Static_assert(NUM_WEREWOLVES < MAX_PLAYERS, "Leave room for one witch");

/*
 * Pick a random index using Linux random bytes
 *
 * Parameters:
 * - int count: Positive number of possible indices
 *
 * Returns:
 * - int index (>= 0): Random index below count
 * - int -1: Random read failed; errno gives the error
 */
static int random_index(int count)
{
    uint32_t value;
    uint32_t limit = UINT32_MAX - UINT32_MAX % (uint32_t)count;

    /* Reject the uneven remainder so every index has the same chance */
    do {
        size_t offset = 0;
        while (offset < sizeof(value)) {
            ssize_t received = getrandom((unsigned char *)&value + offset, // Next output byte
                                         sizeof(value) - offset,           // Bytes still needed
                                         0);                               // Wait for random bytes
            if (received == -1) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (received == 0) {
                errno = EIO;
                return -1;
            }
            offset += (size_t)received;
        }
    } while (value >= limit);

    return (int)(value % (uint32_t)count);
}

/*
 * Randomly assign werewolves, one witch, and townspeople after the lobby
 * The moderator is not in the player array
 *
 * Parameters:
 * - player_t *players: Array of MAX_PLAYERS slots; only connected players join
 *
 * Returns:
 * - int 0: Every connected player has a role
 * - int -1: Too few players or random read failed; roles stay unchanged
 */
static int assign_roles(player_t *players)
{
    int order[MAX_PLAYERS];
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].is_connected) {
            order[count++] = i;
        }
    }

    if (count < NUM_WEREWOLVES + 1) {
        fprintf(stderr, "server: need at least %d players for %d werewolves and one witch; got %d\n",
                NUM_WEREWOLVES + 1, // Minimum player count
                NUM_WEREWOLVES,     // Required werewolves
                count);             // Connected players
        return -1;
    }

    /* Shuffle slot indices without moving player records */
    for (int i = count - 1; i > 0; i--) {
        int chosen = random_index(i + 1); // Remaining indices to choose from
        if (chosen == -1) {
            perror("server: random role assignment");
            return -1;
        }
        int saved = order[i];
        order[i] = order[chosen];
        order[chosen] = saved;
    }

    for (int i = 0; i < count; i++) {
        if (i < NUM_WEREWOLVES) {
            players[order[i]].role = WEREWOLF;
        } else if (i == NUM_WEREWOLVES) {
            players[order[i]].role = WITCH;
        } else {
            players[order[i]].role = TOWNS_PERSON;
        }
    }

    return 0;
}

/*
 * Placeholder for the next game step
 *
 * Parameters:
 * - void: No parameters
 *
 * Returns:
 * - void: Returns immediately
 */
void temp(void)
{
    return;
}

/*
 * Find the time left before the lobby deadline
 *
 * Parameters:
 * - const struct timespec *deadline: Monotonic time when the lobby ends
 *
 * Returns:
 * - int milliseconds (> 0): Time left, rounded up and capped at INT_MAX
 * - int 0: Deadline reached
 * - int -1: Clock read failed; errno gives the error
 */
static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, // Clock unaffected by wall-clock changes
                      &now) == -1) {   // Current time output
        return -1;
    }

    int64_t nanoseconds = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000
                        + deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0) {
        return 0;
    }

    int64_t milliseconds = (nanoseconds + 999999) / 1000000;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

int main(void)
{
    struct sockaddr_un addr;
    comm_addr_setup(&addr); // Server address output

    int server_sockfd = comm_socket_setup();
    if (server_sockfd == -1) {
        perror("server: create socket");
        return EXIT_FAILURE;
    }

    if (comm_server_setup(server_sockfd, // Server socket
                          &addr,         // Server address
                          32) == -1) {   // Pending connection limit
        perror("server: bind/listen");
        if (comm_close(server_sockfd) == -1) {
            perror("server: close listener");
        }
        return EXIT_FAILURE;
    }

    int result = EXIT_SUCCESS;
    player_t players[MAX_PLAYERS] = {0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        players[i].fd = -1;
        players[i].role = ROLE_UNASSIGNED;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, // Elapsed-time clock
                      &deadline) == -1) { // Lobby start time output
        perror("server: read clock");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    deadline.tv_sec += LOBBY_WAITING_TIME;

    printf("Lobby open for %d seconds at %s\n",
           LOBBY_WAITING_TIME, // Lobby duration
           addr.sun_path);    // Socket path
    fflush(stdout);

    for (;;) {
        int timeout_ms = remaining_ms(&deadline); // Lobby deadline
        if (timeout_ms == -1) {
            perror("server: read clock");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        if (timeout_ms == 0) {
            break;
        }

        comm_watch_t watches[MAX_PLAYERS + 1] = {0};
        watches[0].fd = server_sockfd;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            watches[i + 1].fd = players[i].fd;
        }

        if (comm_wait(watches,           // Listener and player sockets
                      MAX_PLAYERS + 1,   // Number of sockets
                      timeout_ms) == -1) { // Time left in the lobby
            perror("server: wait for players");
            result = EXIT_FAILURE;
            goto cleanup;
        }

        /* Lobby clients send no data; input or disconnect releases their slot */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (watches[i + 1].ready) {
                printf("Player left lobby: UID %ju\n",
                       (uintmax_t)players[i].uid); // Departing player
                if (comm_close(players[i].fd) == -1) {
                    perror("server: close player connection");
                    result = EXIT_FAILURE;
                }
                players[i].fd = -1;
                players[i].is_connected = false;
                players[i].is_alive = false;
            }
        }

        if (!watches[0].ready) {
            continue;
        }

        /* Check again so socket activity cannot extend the deadline */
        timeout_ms = remaining_ms(&deadline); // Lobby deadline
        if (timeout_ms == -1) {
            perror("server: read clock");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        if (timeout_ms == 0) {
            break;
        }

        int client_fd = comm_server_accept(server_sockfd); // Ready listener
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                continue;
            }
            perror("server: accept");
            result = EXIT_FAILURE;
            goto cleanup;
        }

        uid_t uid;
        int slot = -1;
        bool duplicate = false;
        if (comm_peer_uid(client_fd, // Accepted player socket
                          &uid) == -1) { // Player UID output
            perror("server: read player UID");
        } else {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_connected && players[i].uid == uid) {
                    duplicate = true;
                }
                if (!players[i].is_connected && slot == -1) {
                    slot = i;
                }
            }
            if (duplicate) {
                puts("Connection rejected: UID already in lobby");
                slot = -1;
            } else if (slot == -1) {
                puts("Connection rejected: lobby full");
            }
        }

        if (slot == -1) {
            if (comm_close(client_fd) == -1) {
                perror("server: close rejected connection");
                result = EXIT_FAILURE;
            }
        } else {
            players[slot] = (player_t){
                .fd = client_fd,
                .uid = uid,
                .role = ROLE_UNASSIGNED,
                .is_connected = true,
                .is_alive = true
            };
            printf("Player joined lobby: UID %ju\n",
                   (uintmax_t)uid); // Accepted player
        }
        fflush(stdout);
    }

    puts("Lobby closed");
    if (assign_roles(players) == -1) { // Connected lobby players
        result = EXIT_FAILURE;
        goto cleanup;
    }
    temp();

cleanup:
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].is_connected && comm_close(players[i].fd) == -1) {
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
