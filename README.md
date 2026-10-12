# Werewolves

A Linux C11 text game in progress. The moderator and clients communicate over a
UNIX domain stream socket. Night and day repeat until one side wins.

## Build and run

For a fresh Ubuntu VM, include `setup.sh` and `remove.sh` when creating the
archive:

```sh
tar -czf werewolves.tgz Makefile README.md setup.sh remove.sh src
```

Copy the archive to the VM, extract it into any directory you own, then run:

```sh
tar -xzf werewolves.tgz
sudo bash setup.sh
```

Use `sudo ./setup.sh --num-players=6` to create `wwplayer1` through
`wwplayer6` instead of the default four players. The accepted range is 2 to 16.
Setup installs missing C build tools, creates the `wwgame` group, `wwmod`, and
the requested distinct player accounts, copies the
project to `/opt/werewolves`, and builds it there. It needs administrator
privileges. Build tools are installed with `apt-get` if needed, so that step
needs package repository access. To choose two werewolves after setup, run
`sudo -u wwmod make -C /opt/werewolves num_werewolves=2`.

Open a terminal for the moderator and each player. In each terminal, switch
to its account and enter the game directory:

```sh
sudo -iu wwmod
cd /opt/werewolves
./server
```

Use `wwplayer1` through the configured player count in the other terminals
and run `./client` there. Start the server last because its lobby
timer begins immediately. At least one werewolf and one witch are required.

When everyone has exited the game and their account shells, run
`sudo bash remove.sh` from the extracted project. The removal script removes
only accounts (including their home directories), the group, and
`/opt/werewolves` if setup created them. It keeps pre-existing accounts,
groups, installations, and build tools. It stops
if any game account still has a running process. It finds setup-created player
accounts by the `wwplayer` prefix in its setup records, regardless of the
player count. If you already installed the
game manually in `/opt/werewolves`, extract the updated archive into that
directory as `wwmod` and run `sudo bash /opt/werewolves/setup.sh`; the removal
script will preserve that earlier installation and its accounts.

For a manual build without account setup:

```sh
make
./server
```

Run `./client` in a separate terminal for each player before the lobby closes.
The server accepts one active connection per Linux UID. For a multiuser run,
each player must use a distinct non-root account in the moderator's socket
group. The moderator owns `/tmp/ww_server`, which players can traverse but
cannot modify; its socket is writable by the group. Several clients under
one UID are useful for testing a rejected duplicate, but do not create
multiple players.

`make num_werewolves=2` builds a server with two werewolves; plain `make` uses
one. `MAX_PLAYERS` is 16 and `LOBBY_WAITING_TIME` is 120 seconds by default.
The moderator is not assigned a role. If fewer than the configured number of
werewolves plus one witch remain at the deadline, the game stops. Run
`make clean` to remove binaries and an owned stale socket; it preserves a live
socket.

## Night

Clients first see `CONNECTED - waiting for game to start...`. After the lobby
closes, every client sees `Entering the night...`, then privately
receives its role and Linux account name.
Werewolves also receive the names they may target: every other connected player,
including other werewolves. A werewolf cannot target themself.
During each werewolf window, the witch and townspeople see
`********SLEEPING********`. Only werewolves receive the chat or vote prompt.

With two or more werewolves, a 30-second chat window comes first. A werewolf
may enter `/chat message`; only the werewolves receive it. With one werewolf,
this chat window is skipped. Wolf messages appear as
`WOLF_CHAT  | wwplayer1: message`. The werewolves then get a separate
30-second vote window. With one werewolf, the vote window starts immediately.
Enter `/vote name` or `/eat name` to choose a
target. A new valid vote replaces that werewolf's previous vote. The server
does not send vote confirmations; invalid commands receive an error. The target
with the most votes is attacked. A tie or no valid votes means no attack.

After voting, werewolves transform back into people and see the sleeping
message. The witch alone learns who was attacked and gets a 30-second window.
The witch may use `/heal` once per game to save that player and `/poison name`
once per game to kill another living player, including a werewolf. The witch cannot poison
themself. A healed player privately receives a dazed awakening message and
returns to sleep. A poisoned player privately learns that they died. Other
players remain asleep during this window.

## Daytime discussion and exile

At daybreak, only living players wake up. They hear which player names died in
the night, or discover wolf tracks if everyone survived. Every living player,
including a werewolf or witch, may discuss for 30 seconds with `/say message`.
Chat goes to all living players, including its sender, without revealing hidden
roles. It appears as `TOWN_CHAT  | wwplayer1: message`; successful chat sends
do not produce an acknowledgment. A separate 30-second vote
window then opens. Use `/exile name` to vote for any
living player, including yourself. A later valid vote replaces your earlier
one. The living player with the most votes is exiled; a tie or no votes means
no exile. Exile is permanent and cannot be healed. If only werewolves remain,
werewolves win. If no werewolves remain and at least one townsperson or witch
remains, townspeople win. A game with no surviving players ends without a
winner. Otherwise another night begins.

The moderator terminal announces night and day directly, then shows `PHASE:`
headings with `|-` chat, accepted votes, witch actions, and results beneath
them. It does not print raw incoming commands or outgoing message queues.
Wolf chat and night choices still appear there, so keep that terminal visible
only to the moderator. Clients see phase names such as
`DAY DISCUSSION 30 seconds`, with blank lines around each phase. Both clients and moderator see
`********* GAME OVER - TOWNSPEOPLE WIN! *********` or the werewolf equivalent.

## Files and communications

- `src/comm.c` is the communications library: socket setup, peer UID lookup,
  readiness waiting, framed reads and writes, queues, and group delivery
- `src/comm.h` declares the communications API and its return values
- `src/server.c` owns player state, role assignment, authorization, phase
  deadlines, chat recipients, votes, and results
- `src/client.c` reads terminal commands and displays server messages
- `src/game.h` defines player and role types
- `Makefile` builds the two executables
- `setup.sh` provisions VM accounts, installs the source, and builds the game
- `remove.sh` removes resources recorded as created by setup

Each wire message is ASCII text preceded by a four-byte unsigned length in
network byte order. Payloads have 1 to 512 bytes and are never raw C structs.
`comm_peer_init` creates per-connection transport state; `comm_peer_read`
receives one frame; `comm_peer_queue` and `comm_peer_queue_group` enqueue
private or group messages; `comm_peer_flush` sends queued bytes without
blocking. `comm_wait` reports readiness for the server and client. The server
uses `comm_peer_uid` to get identity from Linux `SO_PEERCRED` rather than a
client-supplied name. The server looks up that UID's Linux account name for
display and command targets. A successful queue or send does not mean a game
command was accepted. Invalid game commands receive `ERROR`; successful votes
and witch actions do not send acknowledgment messages.
