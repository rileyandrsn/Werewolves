#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <sys/types.h>


typedef enum
{
    ROLE_UNASSIGNED = -1,
    WEREWOLVE,
    WITCH,
    TOWNS_PERSON
} role_t;

typedef struct
{
    int fd; // file descriptor of player
    uid_t uid; // Linux user ID of player
    role_t role; // player's role
    bool is_connected; // indicates whether the player is connected
    bool is_alive; // indicates whether the player is still alive
} player_t;

#endif
