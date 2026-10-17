#define _POSIX_C_SOURCE 200809L // Enable the monotonic clock

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
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

#ifndef DAY_DISCUSSION_SECONDS
#define DAY_DISCUSSION_SECONDS 30
#endif

#ifndef DAY_VOTE_SECONDS
#define DAY_VOTE_SECONDS 30
#endif

_Static_assert(LOBBY_WAITING_TIME >= 0, "Lobby duration must not be negative");
_Static_assert(MAX_PLAYERS > 0, "Player limit must be positive");
_Static_assert(NUM_WEREWOLVES > 0, "There must be at least one werewolf");
_Static_assert(NUM_WEREWOLVES < MAX_PLAYERS, "Leave room for one witch");
_Static_assert(WOLF_CHAT_SECONDS > 0, "Chat window must be positive");
_Static_assert(WOLF_VOTE_SECONDS > 0, "Vote window must be positive");
_Static_assert(WITCH_SECONDS > 0, "Witch window must be positive");
_Static_assert(DAY_DISCUSSION_SECONDS > 0, "Day discussion window must be positive");
_Static_assert(DAY_VOTE_SECONDS > 0, "Day vote window must be positive");

/* Use the account name for display while the kernel UID remains the identity */
static int player_name(uid_t uid, char name[PLAYER_NAME_MAX])
{
    struct passwd *account = getpwuid(uid);
    if (account == NULL || account->pw_name == NULL) {
        errno = ENOENT;
        return -1;
    }
    size_t length = strlen(account->pw_name);
    if (length == 0 || length >= PLAYER_NAME_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        char c = account->pw_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            errno = EINVAL;
            return -1;
        }
    }
    memcpy(name, account->pw_name, length + 1);
    return 0;
}

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
    PHASE_WITCH,
    PHASE_DAY_DISCUSSION,
    PHASE_DAY_VOTE
} phase_t;

typedef struct {
    player_t *players;
    comm_peer_t peers[MAX_PLAYERS];
    int votes[MAX_PLAYERS];
    int day_votes[MAX_PLAYERS];
    int wolf_victim;
    int poison_victim;
    bool healed;
    bool heal_available;
    bool poison_available;
} night_t;

/* Start a new section in the moderator's game log */
static void log_phase(const char *name, int seconds)
{
    if (seconds >= 0) {
        printf("\nPHASE: %s (%d seconds)\n", name, seconds);
    } else {
        printf("\nPHASE: %s\n", name);
    }
    fflush(stdout);
}

/* Close a disconnected player and remove any pending vote from that player */
static void drop_player(night_t *night, int index)
{
    player_t *player = &night->players[index];
    if (!player->is_connected) {
        return;
    }
    printf("|- Player %s disconnected\n", player->name);
    if (comm_close(player->fd) == -1) {
        perror("server: close player");
    }
    player->fd = -1;
    player->is_connected = false;
    player->is_alive = false;
    night->votes[index] = -1;
    night->day_votes[index] = -1;
}

/* Queue a private message and remove a player whose output queue fills */
static void send_player(night_t *night, int index, const char *message)
{
    if (!night->players[index].is_connected) {
        return;
    }
    if (comm_peer_queue(&night->peers[index], message) == -1) {
        drop_player(night, index);
    }
}

/* Queue one message for each selected recipient through the same framing path */
static void send_group(night_t *night, const char *message, bool wolves_only,
                       bool living_only, int exclude)
{
    comm_peer_t *recipients[MAX_PLAYERS];
    int slots[MAX_PLAYERS];
    int results[MAX_PLAYERS];
    size_t count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!night->players[i].is_connected || i == exclude ||
            (wolves_only && night->players[i].role != WEREWOLF) ||
            (living_only && !night->players[i].is_alive)) {
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

/* Tell each player only their own role */
static void announce_roles(night_t *night)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!night->players[i].is_connected) {
            continue;
        }
        const char *role = night->players[i].role == WEREWOLF ? "WEREWOLF" :
                           night->players[i].role == WITCH ? "WITCH" : "TOWNSPERSON";
        char message[COMM_MAX_PAYLOAD + 1];
        snprintf(message, sizeof(message), "ROLE %s NAME %s",
                 role, night->players[i].name);
        send_player(night, i, message);
    }
}

/* Give each living werewolf the current living target names */
static void announce_wolf_targets(night_t *night)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!night->players[i].is_connected || !night->players[i].is_alive ||
            night->players[i].role != WEREWOLF) {
            continue;
        }
        char message[COMM_MAX_PAYLOAD + 1];
        size_t used = (size_t)snprintf(message, sizeof(message), "TARGETS");
        for (int j = 0; j < MAX_PLAYERS; j++) {
            if (j == i || !night->players[j].is_connected ||
                !night->players[j].is_alive) {
                continue;
            }
            size_t name_length = strlen(night->players[j].name);
            if (used + 1 + name_length >= sizeof(message)) {
                send_player(night, i, message);
                used = (size_t)snprintf(message, sizeof(message),
                                        "TARGETS %s", night->players[j].name);
                continue;
            }
            used += (size_t)snprintf(message + used, sizeof(message) - used,
                                     " %s", night->players[j].name);
        }
        send_player(night, i, message);
    }
}

/* Find a connected player by their account name */
static int find_target(night_t *night, int sender, const char *text,
                       bool allow_self)
{
    if (*text == '\0') {
        return -1;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if ((allow_self || i != sender) && night->players[i].is_connected &&
            strcmp(night->players[i].name, text) == 0) {
            return i;
        }
    }
    return -1;
}

/* Validate a command against the sender's role and current phase */
static void handle_command(night_t *night, int sender, phase_t phase,
                           const char *message)
{
    if (phase == PHASE_DAY_DISCUSSION || phase == PHASE_DAY_VOTE) {
        if (!night->players[sender].is_alive || night->players[sender].is_exiled) {
            send_player(night, sender, "ERROR You are out of the game");
            return;
        }
        if (phase == PHASE_DAY_DISCUSSION && strncmp(message, "SAY ", 4) == 0) {
            const char *chat = message + 4;
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
            snprintf(relay, sizeof(relay), "TOWN_CHAT  | %s: %s",
                     night->players[sender].name, chat);
            send_group(night, relay, false, true, -1);
            printf("|- %s\n", relay);
            return;
        }
        if (phase == PHASE_DAY_VOTE && strncmp(message, "EXILE ", 6) == 0) {
            int target = find_target(night, sender, message + 6, true);
            if (target < 0 || !night->players[target].is_alive ||
                night->players[target].is_exiled) {
                send_player(night, sender, "ERROR Choose a living player name");
                return;
            }
            night->day_votes[sender] = target;
            printf("|- %s voted to exile %s\n",
                   night->players[sender].name, night->players[target].name);
            return;
        }
        send_player(night, sender, "ERROR Command is not allowed in this phase");
        return;
    }
    if (phase == PHASE_WITCH) {
        if (night->players[sender].role != WITCH) {
            send_player(night, sender, "ERROR You are sleeping");
            return;
        }
        if (!night->players[sender].is_alive && night->wolf_victim != sender) {
            send_player(night, sender, "ERROR You are out of the game");
            return;
        }
        if (strcmp(message, "HEAL") == 0) {
            int victim = night->wolf_victim;
            if (!night->heal_available || night->healed || victim < 0 ||
                !night->players[victim].is_connected ||
                night->players[victim].is_exiled) {
                send_player(night, sender, "ERROR No player can be healed");
                return;
            }
            night->players[victim].is_alive = true;
            night->healed = true;
            night->heal_available = false;
            printf("|- %s healed %s\n",
                   night->players[sender].name, night->players[victim].name);
            printf("|- %s has been healed\n", night->players[victim].name);
            send_player(night, victim,
                        "You have awoken dazed in your bed; you decide to go back to bed");
            send_player(night, victim, "********SLEEPING********");
            return;
        }
        if (strncmp(message, "POISON ", 7) == 0) {
            if (!night->players[sender].is_alive) {
                send_player(night, sender, "ERROR Heal yourself before poisoning");
                return;
            }
            if (!night->poison_available || night->poison_victim >= 0) {
                send_player(night, sender, "ERROR Poison has already been used");
                return;
            }
            int target = find_target(night, sender, message + 7, false);
            if (target < 0 || !night->players[target].is_alive) {
                send_player(night, sender, "ERROR Choose another living player name");
                return;
            }
            night->poison_victim = target;
            night->poison_available = false;
            night->players[target].is_alive = false;
            printf("|- %s poisoned %s\n",
                   night->players[sender].name, night->players[target].name);
            printf("|- %s has been poisoned\n", night->players[target].name);
            send_player(night, target, "You have been poisoned and died");
            return;
        }
        send_player(night, sender, "ERROR Use /heal or /poison name");
        return;
    }
    if (night->players[sender].role != WEREWOLF ||
        !night->players[sender].is_alive) {
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
        snprintf(relay, sizeof(relay), "WOLF_CHAT  | %s: %s",
                 night->players[sender].name, chat);
        send_group(night, relay, true, true, -1);
        printf("|- %s\n", relay);
        return;
    }
    if (phase == PHASE_VOTE && strncmp(message, "VOTE ", 5) == 0) {
        int target = find_target(night, sender, message + 5, false);
        if (target == -1 || !night->players[target].is_alive) {
            send_player(night, sender, "ERROR Choose another living player name");
            return;
        }
        night->votes[sender] = target;
        printf("|- %s voted to kill %s\n",
               night->players[sender].name, night->players[target].name);
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
    const char *phase_name = phase == PHASE_CHAT ? "werewolf chat" :
                             phase == PHASE_VOTE ? "werewolf vote" :
                             phase == PHASE_WITCH ? "witch" :
                             phase == PHASE_DAY_DISCUSSION ? "day discussion" :
                             "exile vote";
    log_phase(phase_name, seconds);
    char announcement[64];
    snprintf(announcement, sizeof(announcement), "%s %d seconds",
             phase == PHASE_CHAT ? "WEREWOLF CHAT" :
             phase == PHASE_VOTE ? "WEREWOLF VOTE" :
             phase == PHASE_WITCH ? "WITCH" :
             phase == PHASE_DAY_DISCUSSION ? "DAY DISCUSSION" : "EXILE VOTE",
             seconds);
    if (phase == PHASE_WITCH) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!night->players[i].is_connected) {
                continue;
            }
            if (seconds > 0 && night->players[i].role == WITCH &&
                (night->players[i].is_alive || night->wolf_victim == i)) {
                char notice[128];
                if (night->wolf_victim >= 0) {
                    snprintf(notice, sizeof(notice),
                             "You have woken at daybreak and notice %s was killed",
                             night->players[night->wolf_victim].name);
                } else {
                    snprintf(notice, sizeof(notice),
                             "You have woken at daybreak; no player was killed");
                }
                send_player(night, i, announcement);
                send_player(night, i, notice);
                send_player(night, i, "ACTIONS /heal or /poison name");
                send_player(night, i, night->heal_available ?
                            "HEAL AVAILABLE" : "HEAL USED");
                send_player(night, i, night->poison_available ?
                            "POISON AVAILABLE" : "POISON USED");
            } else if (night->players[i].is_alive) {
                if (night->players[i].role == WEREWOLF) {
                    send_player(night, i, "You have transformed back into a person");
                }
                send_player(night, i, "********SLEEPING********");
            }
        }
    } else if (phase == PHASE_DAY_DISCUSSION || phase == PHASE_DAY_VOTE) {
        send_group(night, announcement, false, true, -1);
        send_group(night, phase == PHASE_DAY_DISCUSSION ?
                   "ACTIONS /say message" : "ACTIONS /exile name",
                   false, true, -1);
        if (phase == PHASE_DAY_VOTE) {
            char candidates[COMM_MAX_PAYLOAD + 1] = "CANDIDATES";
            size_t used = strlen(candidates);
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!night->players[i].is_connected || !night->players[i].is_alive) {
                    continue;
                }
                size_t name_length = strlen(night->players[i].name);
                if (used + 1 + name_length >= sizeof(candidates)) {
                    send_group(night, candidates, false, true, -1);
                    used = (size_t)snprintf(candidates, sizeof(candidates),
                                            "CANDIDATES %s", night->players[i].name);
                    continue;
                }
                used += (size_t)snprintf(candidates + used, sizeof(candidates) - used,
                                         " %s", night->players[i].name);
            }
            send_group(night, candidates, false, true, -1);
        }
    } else {
        send_group(night, announcement, true, true, -1);
        send_group(night, phase == PHASE_CHAT ?
                   "ACTIONS /chat message" : "ACTIONS /vote name",
                   true, true, -1);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (night->players[i].is_connected && night->players[i].is_alive &&
                night->players[i].role != WEREWOLF) {
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
        if (night->players[i].is_connected && night->players[i].is_alive &&
            night->players[i].role == WEREWOLF && target >= 0 &&
            night->players[target].is_connected && night->players[target].is_alive) {
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
        puts("|- no players were killed during the night");
        return -1;
    }
    night->players[winner].is_alive = false;
    printf("|- %s has been killed during the night\n",
           night->players[winner].name);
    send_player(night, winner, "You were killed during the night");
    return winner;
}

/* Announce the final night outcome after the witch has acted */
static void announce_daybreak(night_t *night)
{
    char message[256];
    int deaths[2] = {night->wolf_victim, night->poison_victim};
    int dead[2];
    int dead_count = 0;
    for (int i = 0; i < 2; i++) {
        int target = deaths[i];
        if (target < 0 || night->players[target].is_alive ||
            (i == 1 && target == deaths[0])) {
            continue;
        }
        dead[dead_count++] = target;
    }
    if (dead_count == 0) {
        snprintf(message, sizeof(message), "DAYBREAK: no one died");
    } else if (dead_count == 1) {
        snprintf(message, sizeof(message), "DAYBREAK: %s is dead",
                 night->players[dead[0]].name);
    } else {
        snprintf(message, sizeof(message), "DAYBREAK: %s and %s are dead",
                 night->players[dead[0]].name, night->players[dead[1]].name);
    }
    send_group(night, message, false, true, -1);
}

/* Wake only survivors and describe the night without revealing hidden roles */
static void wake_survivors(night_t *night)
{
    int deaths[2] = {night->wolf_victim, night->poison_victim};
    char notice[256];
    int dead_count = 0;
    size_t used = (size_t)snprintf(notice, sizeof(notice),
                                  "You convene with the townspeople outside and discover that");
    for (int i = 0; i < 2; i++) {
        int target = deaths[i];
        if (target < 0 || night->players[target].is_alive ||
            (i == 1 && target == deaths[0])) {
            continue;
        }
        used += (size_t)snprintf(notice + used, sizeof(notice) - used,
                                 "%s%s", dead_count ? " and " : " ",
                                 night->players[target].name);
        dead_count++;
    }
    if (dead_count == 0) {
        snprintf(notice, sizeof(notice),
                 "You go outside and convene with the townspeople and discover wolf tracks");
    } else {
        snprintf(notice + used, sizeof(notice) - used,
                 dead_count == 1 ? " has been killed" : " have been killed");
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (night->players[i].is_connected && night->players[i].is_alive) {
            send_player(night, i, "You have woken up");
            send_player(night, i, notice);
        }
    }
}

/* Count one current vote from each living player and permanently exile a winner */
static void resolve_exile(night_t *night)
{
    int tallies[MAX_PLAYERS] = {0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        int target = night->day_votes[i];
        if (night->players[i].is_connected && night->players[i].is_alive &&
            target >= 0 && night->players[target].is_connected &&
            night->players[target].is_alive) {
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
        send_group(night, "EXILE RESULT NONE", false, true, -1);
        puts("|- No player was exiled");
        return;
    }
    send_player(night, winner, "You have been exiled and cannot return to the game");
    night->players[winner].is_alive = false;
    night->players[winner].is_exiled = true;
    char result[128];
    snprintf(result, sizeof(result), "EXILE RESULT %s",
             night->players[winner].name);
    send_group(night, result, false, true, -1);
    printf("|- %s has been exiled\n", night->players[winner].name);
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

/* Decide the winner from living, connected players */
static const char *winner_message(const night_t *night)
{
    int wolves = 0;
    int townspeople = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!night->players[i].is_connected || !night->players[i].is_alive) {
            continue;
        }
        if (night->players[i].role == WEREWOLF) {
            wolves++;
        } else {
            townspeople++;
        }
    }
    if (wolves == 0 && townspeople == 0) {
        return "NO ONE WINS!";
    }
    if (wolves == 0) {
        return "TOWNSPEOPLE WIN!";
    }
    if (townspeople == 0) {
        return "WEREWOLVES WIN!";
    }
    return NULL;
}

/* Announce the winner once the game reaches a terminal state */
static void announce_winner(night_t *night, const char *winner)
{
    char message[128];
    snprintf(message, sizeof(message), "********* GAME OVER - %s *********", winner);
    puts(message);
    send_group(night, message, false, false, -1);
}

/* Play night and day rounds until one side has no living players */
static int play_game(player_t *players)
{
    night_t night = {
        .players = players,
        .wolf_victim = -1,
        .poison_victim = -1,
        .heal_available = true,
        .poison_available = true
    };
    for (int i = 0; i < MAX_PLAYERS; i++) {
        night.votes[i] = -1;
        night.day_votes[i] = -1;
        if (players[i].is_connected) {
            comm_peer_init(&night.peers[i], players[i].fd);
        }
    }
    bool first_round = true;
    for (;;) {
        night.wolf_victim = -1;
        night.poison_victim = -1;
        night.healed = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            night.votes[i] = -1;
            night.day_votes[i] = -1;
        }
        puts("\nEntering the night...");
        send_group(&night, "Entering the night...", false, true, -1);
        if (first_round) {
            announce_roles(&night);
            first_round = false;
        }
        announce_wolf_targets(&night);
        int living_wolves = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].is_connected && players[i].is_alive &&
                players[i].role == WEREWOLF) {
                living_wolves++;
            }
        }
        if (living_wolves > 1 &&
            run_phase(&night, PHASE_CHAT, WOLF_CHAT_SECONDS) == -1) {
            return -1;
        }
        if (run_phase(&night, PHASE_VOTE, WOLF_VOTE_SECONDS) == -1) {
            return -1;
        }
        night.wolf_victim = resolve_votes(&night);

        bool witch_can_act = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].is_connected && players[i].role == WITCH &&
                ((players[i].is_alive &&
                  (night.heal_available || night.poison_available)) ||
                 (night.wolf_victim == i && night.heal_available))) {
                witch_can_act = true;
            }
        }
        if (run_phase(&night, PHASE_WITCH,
                      witch_can_act ? WITCH_SECONDS : 0) == -1) {
            return -1;
        }
        announce_daybreak(&night);
        const char *winner = winner_message(&night);
        if (winner != NULL) {
            announce_winner(&night, winner);
            break;
        }

        puts("\nEntering the day...");
        wake_survivors(&night);
        if (run_phase(&night, PHASE_DAY_DISCUSSION, DAY_DISCUSSION_SECONDS) == -1) {
            return -1;
        }
        if (run_phase(&night, PHASE_DAY_VOTE, DAY_VOTE_SECONDS) == -1) {
            return -1;
        }
        resolve_exile(&night);
        winner = winner_message(&night);
        if (winner != NULL) {
            announce_winner(&night, winner);
            break;
        }
    }
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

    log_phase("lobby", LOBBY_WAITING_TIME);
    printf("|- Socket at %s\n", addr.sun_path);
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
                printf("|- Player left lobby: %s\n", players[i].name);
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
        char name[PLAYER_NAME_MAX];
        int slot = -1;
        bool duplicate = false;
        if (comm_peer_uid(client_fd, // Accepted player socket
                          &uid) == -1) { // Player UID output
            perror("server: read player UID");
        } else if (player_name(uid, name) == -1) {
            perror("server: read player name");
        } else {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_connected &&
                    (players[i].uid == uid || strcmp(players[i].name, name) == 0)) {
                    duplicate = true;
                }
                if (!players[i].is_connected && slot == -1) {
                    slot = i;
                }
            }
            if (duplicate) {
                puts("|- Connection rejected: account already in lobby");
                slot = -1;
            } else if (slot == -1) {
                puts("|- Connection rejected: lobby full");
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
            memcpy(players[slot].name, name, strlen(name) + 1);
            printf("|- Player joined lobby: %s\n", name);
        }
        fflush(stdout);
    }

    puts("|- Lobby closed");
    if (assign_roles(players) == -1) { // Connected lobby players
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (play_game(players) == -1) { // Connected players with assigned roles
        perror("server: game");
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

    puts("Server has closed all player connections");

    return result;
}
