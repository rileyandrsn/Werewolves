# Werewolves

A Linux C11 text game in progress. The moderator and clients communicate over a
UNIX domain stream socket. Night and day repeat until one side wins.

## Build and run

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

After the lobby closes, every client sees `Entering the night`, then privately
receives its role and UID.
Werewolves also receive the UIDs they may target: every other connected player,
including other werewolves. A werewolf cannot target themself.
During each werewolf window, the witch and townspeople see
`********SLEEPING********`. Only werewolves receive the chat or vote prompt.

With two or more werewolves, a 30-second chat window comes first. A werewolf
may enter `/chat message`; only the other werewolves receive it. All
werewolves then get a separate 30-second vote window. With one werewolf, the
vote window starts immediately. Enter `/vote UID` or `/eat UID` to choose a
target. A new valid vote replaces that werewolf's previous vote. The server
acknowledges accepted commands; invalid commands receive an error. The target
with the most votes is attacked. A tie or no valid votes means no attack.

After voting, werewolves transform back into people and see the sleeping
message. The witch alone learns who was attacked and gets a 30-second window.
The witch may use `/heal` once per game to save that player and `/poison UID`
once per game to kill another living player, including a werewolf. The witch cannot poison
themself. A healed player privately receives a dazed awakening message and
returns to sleep. A poisoned player privately learns that they died. Other
players remain asleep during this window.

## Daytime discussion and exile

At daybreak, only living players wake up. They hear which player UIDs died in
the night, or discover wolf tracks if everyone survived. Every living player,
including a werewolf or witch, may discuss for 30 seconds with `/say message`.
Chat goes to the other living players without revealing hidden roles. A
separate 30-second vote window then opens. Use `/exile UID` to vote for any
living player, including yourself. A later valid vote replaces your earlier
one. The living player with the most votes is exiled; a tie or no votes means
no exile. Exile is permanent and cannot be healed. If only werewolves remain,
werewolves win. If no werewolves remain and at least one townsperson or witch
remains, townspeople win. A game with no surviving players ends without a
winner. Otherwise another night begins.

## Files and communications

- `src/comm.c` is the communications library: socket setup, peer UID lookup,
  readiness waiting, framed reads and writes, queues, and group delivery
- `src/comm.h` declares the communications API and its return values
- `src/server.c` owns player state, role assignment, authorization, phase
  deadlines, chat recipients, votes, and results
- `src/client.c` reads terminal commands and displays server messages
- `src/game.h` defines player and role types
- `Makefile` builds the two executables

Each wire message is ASCII text preceded by a four-byte unsigned length in
network byte order. Payloads have 1 to 512 bytes and are never raw C structs.
`comm_peer_init` creates per-connection transport state; `comm_peer_read`
receives one frame; `comm_peer_queue` and `comm_peer_queue_group` enqueue
private or group messages; `comm_peer_flush` sends queued bytes without
blocking. `comm_wait` reports readiness for the server and client. The server
uses `comm_peer_uid` to get identity from Linux `SO_PEERCRED` rather than a
client-supplied name. A successful queue or send does not mean a game command
was accepted; the server sends `ACK` or `ERROR` for that.
