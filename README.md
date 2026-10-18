# Werewolves

Werewolves is a Linux CLI game based on the game Mafia. The moderator and clients communicate over a
UNIX domain stream socket. 

See ```Build and run``` for instructions on installing the game.
See ```Gameplay Guide``` for instructions on playing the game.

## Build and run

For a fresh Ubuntu VM, include `setup.sh` and `remove.sh` when creating the
archive (step not necessary if you already have the gzipped tar file):

```sh
tar -czf werewolves.tgz Makefile README.md setup.sh remove.sh src
```

Copy the archive to a Linux VM, extract it into any directory you own, then run:

```sh
tar -xzf werewolves.tgz
sudo bash setup.sh
```

Use `sudo ./setup.sh --num-players=6` to create `wwplayer1` through
`wwplayer6` instead of the default four players. The accepted range is 2 to 16.

The setup script:
* installs missing C build tools 
* creates the `wwgame` group, `wwmod`, and the requested distinct player accounts
* copies the project to `/opt/werewolves` and builds it there

It needs administrator privileges. Build tools are installed with `apt-get` if needed, so that step
needs package repository access. 
To choose two werewolves after setup, run
`sudo -u wwmod make -C /opt/werewolves num_werewolves=2`.

Open a terminal for the moderator and each player. In each terminal, switch
to its account and enter the game directory:

```sh
sudo -iu wwmod
cd /opt/werewolves
./server
```

Use `wwplayer1` through the configured player count in the other terminals
and run `./client` there.

When everyone has exited the game and their account shells, run
`sudo bash remove.sh` 
from the extracted project. The removal script removes 
* only accounts (including their home directories)
* the group 
* and `/opt/werewolves` if setup created them. 

It keeps pre-existing accounts, groups, installations, and build tools. It stops
if any game account still has a running process. It finds setup-created player accounts by the `wwplayer` prefix in its setup records, regardless of the player count. If you already installed the game manually in `/opt/werewolves`, extract the updated archive into that directory as `wwmod` and run `sudo bash /opt/werewolves/setup.sh`; the removal script will preserve that earlier installation and its accounts.



## Gameplay Guide

### Lobby
The server creates a lobby that gives players 120 seconds to connect to the server.

### Werewolf / Nighttime Phases
After the lobby closes, every player enters the nighttime phase, then privately receives its role. Werewolves also receive the names they may target: every other connected player, including other werewolves. A werewolf cannot target themself. During each werewolf window, the witch and townspeople are asleep. Only werewolves receive the chat or vote prompt.

With two or more werewolves, a 30-second chat window comes first. A werewolf may enter `/chat <message>` 
only the werewolves receive it. With one werewolf, this chat window is skipped.

The werewolves then get a separate 30-second vote window. With one werewolf, the vote window starts immediately.
Enter `/vote name` or `/eat name` to choose a target. A new valid vote replaces that werewolf's previous vote. The target
with the most votes is attacked. A tie or no valid votes means no player is attacked.

### Witch Phase
After voting, werewolves transform back into people. The witch alone learns who was attacked and gets a 30-second window.
The witch may use `/heal` once per game to save that player and `/poison name` once per game to kill another living player, including a werewolf. The witch cannot poison themself. A healed player privately receives a dazed awakening message and
returns to sleep. A poisoned player privately learns that they died. Other players remain asleep during this window.

## Daytime discussion and exile

At daybreak, only living players wake up. They hear which player names died in the night, or discover wolf tracks if everyone survived. Every living player, including a werewolf or witch, may discuss for 30 seconds with `/say <message>`.
A separate 30-second vote window then opens. All players are given the opportunity to vote on which player should be exiled from the town, using `/exile <name>` to vote for any living player; a later valid vote replaces your earlier one. 
The living player with the most votes is exiled; a tie or no votes means no exile. Exile is permanent and cannot be healed. If only werewolves remain, werewolves win. If no werewolves remain and at least one townsperson or witch
remains, townspeople win. A game with no surviving players ends without a winner. Otherwise another night begins and the night/day cycle repeats.


## Files and Communication
The communications library is implemented in ```src/comm.c```, with its public interface in ```src/comm.h```.

Werewolves uses a UNIX domain stream socket for inter-process communication (IPC) between the server and client processes. It provides functionality for establishing client-server connections, message transmission and receipt, and communication management.

| Function                | Description                                                                     |
| ----------------------- | ------------------------------------------------------------------------------- |
| comm_peer_init()        | Initializes the communication state, including buffers for the connected socket |   
| comm_peer_read()        | Reads a message from a socket                                                   |   
| comm_peer_queue()       | Queues a message for transmission                                               |   
| comm_peer_queue_group() | Queues a message to be transmitted to multiple recipients                       |   
| comm_peer_flush()       | Transmits queued data to a connected client                                     |   
| comm_peer_has_output()  | Checks if a client has pending data in its buffer                               |   
| comm_wait()             | Monitors multiple sockets for activity using poll()                             |   
| comm_socket_setup()     | Creates a UNIX domain stream socket                                             |   
| comm_addr_setup()       | Initializes the server socket address                                           |   
| comm_server_setup()     | Binds the server socket to its address, listens for incoming client connections |   
| comm_server_accept()    | Accepts one incoming client connection                                          |   
| comm_peer_uid()         | Retrieves the peer's Linux UID                                                  |   
| comm_client_connect()   | Connects a client to the server                                                 |   
| comm_close()            | Closes a socket file descriptor                                                 |   
| comm_server_cleanup()   | Cleans up the server's socket path                                              |   


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


