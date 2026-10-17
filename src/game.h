#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <sys/types.h>

#define PLAYER_NAME_MAX 64


typedef enum
{
    ROLE_UNASSIGNED = -1,
    WEREWOLF,
    WITCH,
    TOWNS_PERSON
} role_t;

typedef struct
{
    int fd; // file descriptor of player's connection
    uid_t uid; // user ID of player
    char name[PLAYER_NAME_MAX]; // name of the player's account
    role_t role; // player's role
    bool is_connected; // indicates whether the player is connected
    bool is_alive; // indicates whether the player is still alive
    bool is_exiled; // indicates whether the player has been exiled; permanent removal from the game
} player_t;

#endif
