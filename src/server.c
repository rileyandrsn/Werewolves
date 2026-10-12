#define _POSIX_C_SOURCE 200809L // Enable the monotonic clock

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#ifndef WOLF_CHAT_SECONDS
#define WOLF_CHAT_SECONDS 30
#endif

#ifndef WOLF_VOTE_SECONDS
#define WOLF_VOTE_SECONDS 30
#endif

#ifndef WITCH_SECONDS
#define WITCH_SECONDS 30
#endif

_Static_assert(LOBBY_WAITING_TIME >= 0, "Lobby duration must not be negative");
_Static_assert(MAX_PLAYERS > 0, "Player limit must be positive");
_Static_assert(NUM_WEREWOLVES > 0, "There must be at least one werewolf");
_Static_assert(NUM_WEREWOLVES < MAX_PLAYERS, "Leave room for one witch");
_Static_assert(WOLF_CHAT_SECONDS > 0, "Chat window must be positive");
_Static_assert(WOLF_VOTE_SECONDS > 0, "Vote window must be positive");
_Static_assert(WITCH_SECONDS > 0, "Witch window must be positive");

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

typedef enum {
    PHASE_CHAT,
    PHASE_VOTE,
    PHASE_WITCH
} phase_t;

typedef struct {
    player_t *players;
    comm_peer_t peers[MAX_PLAYERS];
    int votes[MAX_PLAYERS];
    int wolf_victim;
    int poison_victim;
    bool healed;
} night_t;

/* Close a disconnected player and remove any pending vote from that player */
static void drop_player(night_t *night, int index)
{
    player_t *player = &night->players[index];
    if (!player->is_connected) {
        return;
    }
    if (comm_close(player->fd) == -1) {
        perror("server: close player");
    }
    player->fd = -1;
    player->is_connected = false;
    player->is_alive = false;
    night->votes[index] = -1;
}

/* Queue a private message and remove a player whose output queue fills */
static void send_player(night_t *night, int index, const char *message)
{
    if (night->players[index].is_connected &&
        comm_peer_queue(&night->peers[index], message) == -1) {
        drop_player(night, index);
    }
}

/* Queue one message for each selected recipient through the same framing path */
static void send_group(night_t *night, const char *message, bool wolves_only,
                       int exclude)
{
    comm_peer_t *recipients[MAX_PLAYERS];
    int slots[MAX_PLAYERS];
    int results[MAX_PLAYERS];
    size_t count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!night->players[i].is_connected || i == exclude ||
            (wolves_only && night->players[i].role != WEREWOLF)) {
            continue;
        }
        recipients[count] = &night->peers[i];
        slots[count++] = i;
    }
    if (count == 0) {
        return;
    }
    comm_peer_queue_group(recipients, count, message, results);
    for (size_t i = 0; i < count; i++) {
        if (results[i] == -1) {
            drop_player(night, slots[i]);
        }
    }
}

/* Tell each player only their own role; give wolves the valid target UIDs */
static void announce_roles(night_t *night)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!night->players[i].is_connected) {
            continue;
        }
        const char *role = night->players[i].role == WEREWOLF ? "WEREWOLF" :
                           night->players[i].role == WITCH ? "WITCH" : "TOWNSPERSON";
        char message[COMM_MAX_PAYLOAD + 1];
        snprintf(message, sizeof(message), "ROLE %s UID %ju",
                 role, (uintmax_t)night->players[i].uid);
        send_player(night, i, message);
        if (!night->players[i].is_connected || night->players[i].role != WEREWOLF) {
            continue;
        }
        size_t used = (size_t)snprintf(message, sizeof(message), "TARGETS");
        for (int j = 0; j < MAX_PLAYERS; j++) {
            if (j == i || !night->players[j].is_connected) {
                continue;
            }
            int written = snprintf(message + used, sizeof(message) - used,
                                   " %ju", (uintmax_t)night->players[j].uid);
            if (written < 0 || (size_t)written >= sizeof(message) - used) {
                break;
            }
            used += (size_t)written;
        }
        send_player(night, i, message);
    }
}

/* Parse a UID and find a connected target other than the sender */
static int find_target(night_t *night, int sender, const char *text)
{
    if (*text == '\0') {
        return -1;
    }
    for (const char *digit = text; *digit; digit++) {
        if (*digit < '0' || *digit > '9') {
            return -1;
        }
    }
    errno = 0;
    char *end;
    uintmax_t requested = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || requested > (uintmax_t)(uid_t)-1) {
        return -1;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != sender && night->players[i].is_connected &&
            night->players[i].uid == (uid_t)requested) {
            return i;
        }
    }
    return -1;
}

/* Validate a command against the sender's role and current phase */
static void handle_command(night_t *night, int sender, phase_t phase,
                           const char *message)
{
    if (phase == PHASE_WITCH) {
        if (night->players[sender].role != WITCH) {
            send_player(night, sender, "ERROR You are sleeping");
            return;
        }
        if (strcmp(message, "HEAL") == 0) {
            int victim = night->wolf_victim;
            if (night->healed || victim < 0 || !night->players[victim].is_connected) {
                send_player(night, sender, "ERROR No player can be healed");
                return;
            }
            night->players[victim].is_alive = true;
            night->healed = true;
            send_player(night, sender, "ACK HEAL");
            send_player(night, victim,
                        "You have awoken dazed in your bed; you decide to go back to bed");
            send_player(night, victim, "********SLEEPING********");
            return;
        }
        if (strncmp(message, "POISON ", 7) == 0) {
            if (night->poison_victim >= 0) {
                send_player(night, sender, "ERROR Poison has already been used");
                return;
            }
            int target = find_target(night, sender, message + 7);
            if (target < 0 || !night->players[target].is_alive) {
                send_player(night, sender, "ERROR Choose another living player UID");
                return;
            }
            night->poison_victim = target;
            night->players[target].is_alive = false;
            send_player(night, sender, "ACK POISON");
            send_player(night, target, "You have been poisoned and died");
            return;
        }
        send_player(night, sender, "ERROR Use /heal or /poison UID");
        return;
    }
    if (night->players[sender].role != WEREWOLF) {
        send_player(night, sender, "ERROR Only werewolves can act at night");
        return;
    }
    if (phase == PHASE_CHAT && strncmp(message, "CHAT ", 5) == 0) {
        const char *chat = message + 5;
        size_t length = strlen(chat);
        if (length == 0 || length > 200) {
            send_player(night, sender, "ERROR Chat must be 1 to 200 characters");
            return;
        }
        for (size_t i = 0; i < length; i++) {
            if ((unsigned char)chat[i] < 32 || (unsigned char)chat[i] > 126) {
                send_player(night, sender, "ERROR Use printable ASCII in chat");
                return;
            }
        }
        char relay[COMM_MAX_PAYLOAD + 1];
        snprintf(relay, sizeof(relay), "WOLF_CHAT %ju %s",
                 (uintmax_t)night->players[sender].uid, chat);
        send_group(night, relay, true, sender);
        send_player(night, sender, "ACK CHAT");
        return;
    }
    if (phase == PHASE_VOTE && strncmp(message, "VOTE ", 5) == 0) {
        int target = find_target(night, sender, message + 5);
        if (target == -1) {
            send_player(night, sender, "ERROR Choose another connected player UID");
            return;
        }
        night->votes[sender] = target;
        send_player(night, sender, "ACK VOTE");
        return;
    }
    send_player(night, sender, "ERROR Command is not allowed in this phase");
}

/* Run a phase until its monotonic deadline while servicing every player */
static int run_phase(night_t *night, phase_t phase, int seconds)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) {
        return -1;
    }
    deadline.tv_sec += seconds;
    char announcement[64];
    snprintf(announcement, sizeof(announcement), "PHASE %s %d",
             phase == PHASE_CHAT ? "CHAT" : phase == PHASE_VOTE ? "VOTE" : "WITCH",
             seconds);
    if (phase == PHASE_WITCH) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!night->players[i].is_connected) {
                continue;
            }
            if (night->players[i].role == WITCH) {
                char notice[128];
                if (night->wolf_victim >= 0) {
                    snprintf(notice, sizeof(notice),
                             "You have woken at daybreak and notice player UID %ju was killed",
                             (uintmax_t)night->players[night->wolf_victim].uid);
                } else {
                    snprintf(notice, sizeof(notice),
                             "You have woken at daybreak; no player was killed");
                }
                send_player(night, i, notice);
                send_player(night, i, announcement);
                send_player(night, i, "ACTIONS /heal or /poison UID");
            } else {
                if (night->players[i].role == WEREWOLF) {
                    send_player(night, i, "You have transformed back into a person");
                }
                send_player(night, i, "********SLEEPING********");
            }
        }
    } else {
        send_group(night, announcement, true, -1);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (night->players[i].is_connected && night->players[i].role != WEREWOLF) {
                send_player(night, i, "********SLEEPING********");
            }
        }
    }

    for (;;) {
        int timeout_ms = remaining_ms(&deadline);
        if (timeout_ms <= 0) {
            return timeout_ms;
        }
        comm_watch_t watches[MAX_PLAYERS] = {0};
        for (int i = 0; i < MAX_PLAYERS; i++) {
            watches[i].fd = night->players[i].is_connected ? night->players[i].fd : -1;
            watches[i].want_write = night->players[i].is_connected &&
                                    comm_peer_has_output(&night->peers[i]);
        }
        if (comm_wait(watches, MAX_PLAYERS, timeout_ms) == -1) {
            return -1;
        }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!night->players[i].is_connected) {
                continue;
            }
            if (watches[i].writable && comm_peer_flush(&night->peers[i]) == -1) {
                drop_player(night, i);
                continue;
            }
            if (!watches[i].readable) {
                continue;
            }
            for (int frame = 0; frame < 8; frame++) {
                char message[COMM_MAX_PAYLOAD + 1];
                int status = comm_peer_read(&night->peers[i], message, sizeof(message));
                if (status == 0) {
                    break;
                }
                if (status != 1) {
                    drop_player(night, i);
                    break;
                }
                if (remaining_ms(&deadline) <= 0) {
                    break;
                }
                handle_command(night, i, phase, message);
                if (!night->players[i].is_connected) {
                    break;
                }
            }
        }
    }
}

/* Resolve plurality votes; a tie or no votes means no one is eaten */
static int resolve_votes(night_t *night)
{
    int tallies[MAX_PLAYERS] = {0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        int target = night->votes[i];
        if (night->players[i].is_connected && night->players[i].role == WEREWOLF &&
            target >= 0 && night->players[target].is_connected) {
            tallies[target]++;
        }
    }
    int winner = -1;
    int highest = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (tallies[i] > highest) {
            highest = tallies[i];
            winner = i;
        } else if (tallies[i] != 0 && tallies[i] == highest) {
            winner = -1;
        }
    }
    if (winner == -1) {
        return -1;
    }
    night->players[winner].is_alive = false;
    return winner;
}

/* Announce the final night outcome after the witch has acted */
static void announce_daybreak(night_t *night)
{
    char message[128] = "DAYBREAK";
    int deaths[2] = {night->wolf_victim, night->poison_victim};
    size_t used = strlen(message);
    int dead_count = 0;
    for (int i = 0; i < 2; i++) {
        int target = deaths[i];
        if (target < 0 || night->players[target].is_alive ||
            (i == 1 && target == deaths[0])) {
            continue;
        }
        used += (size_t)snprintf(message + used, sizeof(message) - used,
                                 " DEAD %ju", (uintmax_t)night->players[target].uid);
        dead_count++;
    }
    if (dead_count == 0) {
        snprintf(message, sizeof(message), "DAYBREAK No one died");
    }
    send_group(night, message, false, -1);
    puts("Witch phase ended; daybreak result sent");
}

/* Flush final messages for up to one second without waiting on a slow player */
static void flush_results(night_t *night)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) {
        return;
    }
    deadline.tv_sec++;
    for (;;) {
        comm_watch_t watches[MAX_PLAYERS] = {0};
        bool pending = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            watches[i].fd = night->players[i].is_connected ? night->players[i].fd : -1;
            watches[i].want_write = night->players[i].is_connected &&
                                    comm_peer_has_output(&night->peers[i]);
            pending |= watches[i].want_write;
        }
        int timeout = remaining_ms(&deadline);
        if (!pending || timeout <= 0 || comm_wait(watches, MAX_PLAYERS, timeout) == -1) {
            return;
        }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (night->players[i].is_connected && watches[i].writable &&
                comm_peer_flush(&night->peers[i]) == -1) {
                drop_player(night, i);
            }
        }
    }
}

/* Play the first night phase and return 0 on success or -1 on wait failure */
static int first_night(player_t *players)
{
    night_t night = {.players = players, .wolf_victim = -1, .poison_victim = -1};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        night.votes[i] = -1;
        if (players[i].is_connected) {
            comm_peer_init(&night.peers[i], players[i].fd);
        }
    }
    send_group(&night, "Entering the night", false, -1);
    announce_roles(&night);
    if (NUM_WEREWOLVES > 1 && run_phase(&night, PHASE_CHAT, WOLF_CHAT_SECONDS) == -1) {
        return -1;
    }
    if (run_phase(&night, PHASE_VOTE, WOLF_VOTE_SECONDS) == -1) {
        return -1;
    }
    night.wolf_victim = resolve_votes(&night);
    if (run_phase(&night, PHASE_WITCH, WITCH_SECONDS) == -1) {
        return -1;
    }
    announce_daybreak(&night);
    flush_results(&night);
    return 0;
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
    if (first_night(players) == -1) { // Connected players with assigned roles
        perror("server: first night");
        result = EXIT_FAILURE;
    }

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
