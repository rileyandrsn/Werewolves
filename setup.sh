#!/usr/bin/env bash
set -euo pipefail

# Run as root after extracting the project on an Ubuntu VM
if (( EUID != 0 )); then
    echo "Run this script with sudo: sudo bash setup.sh" >&2
    exit 1
fi

num_players=4
if (( $# == 1 )); then
    if [[ $1 =~ ^--num-players=([0-9]{1,2})$ ]]; then
        num_players=$((10#${BASH_REMATCH[1]}))
    else
        echo "Usage: sudo ./setup.sh [--num-players=N] (N must be 2 to 16)" >&2
        exit 1
    fi
elif (( $# != 0 )); then
    echo "Usage: sudo ./setup.sh [--num-players=N] (N must be 2 to 16)" >&2
    exit 1
fi
if (( num_players < 2 || num_players > 16 )); then
    echo "Number of players must be between 2 and 16" >&2
    exit 1
fi

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
install_dir=/opt/werewolves
state_dir=/var/lib/werewolves-setup
game_group=wwgame
moderator=wwmod
players=()
for ((i = 1; i <= num_players; i++)); do
    players+=("wwplayer$i")
done

for file in Makefile README.md remove.sh src/client.c src/comm.c src/comm.h src/game.h src/server.c; do
    if [[ ! -f "$source_dir/$file" ]]; then
        echo "Missing $source_dir/$file; run setup from the extracted project" >&2
        exit 1
    fi
done

if [[ -e "$state_dir" || -L "$state_dir" ]]; then
    if [[ -L "$state_dir" || ! -d "$state_dir" ||
          $(stat -c %u -- "$state_dir") != 0 ||
          $(stat -c %a -- "$state_dir") != 700 ||
          ! -f "$state_dir/managed" ]]; then
        echo "Refusing an unrecognized setup state at $state_dir" >&2
        exit 1
    fi
fi
if [[ -e "$install_dir" || -L "$install_dir" ]]; then
    if [[ -L "$install_dir" || ! -d "$install_dir" ]]; then
        echo "Refusing a non-directory installation path at $install_dir" >&2
        exit 1
    fi
    if [[ ! -f "$state_dir/created-install" && "$source_dir" != "$install_dir" ]]; then
        echo "$install_dir already exists outside this setup" >&2
        echo "Run setup from that directory, or move the existing installation aside first" >&2
        exit 1
    fi
fi

if ! command -v make >/dev/null || ! command -v gcc >/dev/null; then
    if ! command -v apt-get >/dev/null; then
        echo "Install make and gcc, then rerun setup" >&2
        exit 1
    fi
    echo "Installing C build tools"
    apt-get update
    apt-get install -y build-essential
fi

if [[ ! -d "$state_dir" ]]; then
    install -d -m 0700 -- "$state_dir"
    touch -- "$state_dir/managed"
fi

if ! getent group "$game_group" >/dev/null; then
    groupadd "$game_group"
    touch -- "$state_dir/created-group"
fi

for account in "$moderator" "${players[@]}"; do
    if ! getent passwd "$account" >/dev/null; then
        useradd -m -g "$game_group" -s /bin/bash "$account"
        touch -- "$state_dir/created-user-$account"
    fi
done

# The socket inherits the moderator's primary group
if [[ $(id -gn "$moderator") != "$game_group" ]]; then
    echo "$moderator must have $game_group as its primary group" >&2
    exit 1
fi
group_gid=$(getent group "$game_group" | cut -d: -f3)
for account in "${players[@]}"; do
    if [[ " $(id -G "$account") " != *" $group_gid "* ]]; then
        echo "$account must belong to $game_group" >&2
        exit 1
    fi
done

if [[ -d "$install_dir" &&
      ( $(stat -c %U -- "$install_dir") != "$moderator" ||
        $(stat -c %G -- "$install_dir") != "$game_group" ) ]]; then
    echo "$install_dir must be owned by $moderator:$game_group" >&2
    exit 1
fi
if [[ -L "$install_dir/src" ]]; then
    echo "Refusing a linked source directory at $install_dir/src" >&2
    exit 1
fi
for file in Makefile README.md setup.sh remove.sh \
            src/client.c src/comm.c src/comm.h src/game.h src/server.c; do
    if [[ -L "$install_dir/$file" ]]; then
        echo "Refusing a linked installation file at $install_dir/$file" >&2
        exit 1
    fi
done

if [[ ! -d "$install_dir" ]]; then
    touch -- "$state_dir/created-install"
    install -d -o "$moderator" -g "$game_group" -m 0755 -- "$install_dir"
fi

if [[ "$source_dir" != "$install_dir" ]]; then
    install -d -o "$moderator" -g "$game_group" -m 0755 -- "$install_dir/src"
    for file in Makefile README.md; do
        install -o "$moderator" -g "$game_group" -m 0644 \
            -- "$source_dir/$file" "$install_dir/$file"
    done
    for file in src/client.c src/comm.c src/comm.h src/game.h src/server.c; do
        install -o "$moderator" -g "$game_group" -m 0644 \
            -- "$source_dir/$file" "$install_dir/$file"
    done
    for file in setup.sh remove.sh; do
        install -o "$moderator" -g "$game_group" -m 0755 \
            -- "$source_dir/$file" "$install_dir/$file"
    done
fi

runuser -u "$moderator" -- make -C "$install_dir"

echo "Setup complete"
echo "Open a terminal for each account with sudo -iu wwmod or sudo -iu wwplayer1 (through wwplayer$num_players)"
echo "In each terminal: cd /opt/werewolves"
echo "Start ./server as wwmod, then start ./client as each player within the 120-second lobby"
