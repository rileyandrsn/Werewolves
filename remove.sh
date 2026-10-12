#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "Run this script with sudo: sudo bash remove.sh" >&2
    exit 1
fi

install_dir=/opt/werewolves
state_dir=/var/lib/werewolves-setup
game_group=wwgame
moderator=wwmod
players=()
while IFS=: read -r account _; do
    if [[ $account =~ ^wwplayer[0-9]+$ ]]; then
        players+=("$account")
    fi
done < <(getent passwd)

if [[ -L "$state_dir" || ! -d "$state_dir" ||
      $(stat -c %u -- "$state_dir" 2>/dev/null) != 0 ||
      $(stat -c %a -- "$state_dir" 2>/dev/null) != 700 ||
      ! -f "$state_dir/managed" ]]; then
    echo "No recognized setup state at $state_dir; nothing was removed" >&2
    exit 1
fi

# Do not remove a live game or an account with an open terminal
for account in "$moderator" "${players[@]}"; do
    if getent passwd "$account" >/dev/null && pgrep -u "$account" >/dev/null; then
        echo "$account has running processes; exit its terminals and stop the game first" >&2
        exit 1
    fi
done

if [[ -f "$state_dir/created-install" ]]; then
    if [[ -L "$install_dir" || ! -d "$install_dir" ||
          $(stat -c %U -- "$install_dir") != "$moderator" ]]; then
        echo "Installation path changed; refusing to remove $install_dir" >&2
        exit 1
    fi
    runuser -u "$moderator" -- make -C "$install_dir" clean
    rm -rf -- "$install_dir"
    rm -- "$state_dir/created-install"
fi

shopt -s nullglob
for marker in "$state_dir"/created-user-wwplayer*; do
    account=${marker##*/}
    account=${account#created-user-}
    if [[ ! $account =~ ^wwplayer[1-9][0-9]*$ ]]; then
        echo "Unexpected player record at $marker; stopping" >&2
        exit 1
    fi
    if getent passwd "$account" >/dev/null; then
        userdel -r "$account"
    fi
    rm -- "$marker"
done
if [[ -f "$state_dir/created-user-$moderator" ]]; then
    if getent passwd "$moderator" >/dev/null; then
        userdel -r "$moderator"
    fi
    rm -- "$state_dir/created-user-$moderator"
fi

if [[ -f "$state_dir/created-group" ]]; then
    groupdel "$game_group"
    rm -- "$state_dir/created-group"
fi

rm -- "$state_dir/managed"
rmdir -- "$state_dir"
echo "Setup-created game files, accounts, and group removed"
echo "Pre-existing accounts and build tools were kept"
