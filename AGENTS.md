# Werewolves Game — Agent Instructions

## Purpose and current direction

This is my CSE 536 project: an original, local, text-based
Werewolves-like game on Linux. The project is both a game and an exercise in
understanding interprocess communication. Help Riley build it incrementally
and understand the implementation.

- Implement in C, targeting Linux and C11.
- Use UNIX domain sockets (`AF_UNIX`, `SOCK_STREAM`) for IPC.
- Put source files in `src/` and the Makefile at the project root.
- Earlier design material proposed Python. C is the current decision; do not
  introduce Python or asyncio into the game implementation.
- Proposed roles, phase durations, player counts, and reconnect policies are
  design suggestions, not assignment requirements. Follow the current code
  and my latest decisions when these differ.

## Assignment requirements

Preserve these requirements throughout development:

- Develop an original codebase specifically for this class.
- Every player must be a separate process running as a distinct Linux user.
  Several terminals under one UID are useful for development but do not
  satisfy the final demonstration requirement.
- Keep the communications library implementation in a single source file.
- Use IPC more sophisticated than named pipes.
- Include non-trivial strategy, concurrency, player-to-player communication,
  and group communication such as multicast or broadcast.
- Final submission is a gzip-compressed `.tgz` containing all necessary source,
  a README, and a PDF report. No Word report.

## Architecture and file responsibilities

Use this as the intended layout; do not create unused files just to match it:

| File | Responsibility |
| --- | --- |
| `src/comm.c` | All IPC implementation: socket setup, connection handling, peer credentials, framing, transport buffers, sending, receiving, and group delivery. |
| `src/comm.h` | Public declarations, shared communication types, and API contracts. No nontrivial function implementations. |
| `src/server.c` | Moderator entry point, connection-to-player mapping, game orchestration, authorization, and recipient selection. |
| `src/client.c` | Player entry point, terminal commands, and message display. |
| `src/game.c`, `src/game.h` | Optional separation for roles, phases, votes, resolution, and victory rules when the implementation warrants it. |
| `Makefile` | Build server and client from `src/`. |
| `README.md` | Setup, build, gameplay, file map, and communications API documentation. |

The moderator owns authoritative game state. Clients submit requests; they
cannot assign roles, declare results, or bypass server-side validation.

Keep socket operations and IPC handling behind `comm.c`. Server and client
code should call its public API rather than duplicate `socket`, `bind`,
`listen`, `accept`, `connect`, `send`, or `recv` logic. Hide transport readiness
handling behind the library as concurrency develops.

Keep game-specific decisions outside `comm.c`: the server chooses wolf-chat
recipients, validates votes, and resolves night actions. The library sends to
opaque connections or supplied recipient lists without knowing player roles.

## Communications design

- Start with a small working connection example, then framed messages, then
  multiple-player concurrency. A blocking prototype is an intermediate step.
- UNIX stream sockets carry bytes, not discrete application messages. Handle
  short reads and writes; never assume one `send` matches one `recv`.
- The intended framing is a 4-byte unsigned payload length in network byte
  order followed by the payload bytes. Define and enforce a maximum size.
- Document payload encoding. Do not transmit raw C structs as a wire format:
  padding, layout, and representation should not become protocol assumptions.
- Distinguish clean EOF from a truncated frame. Handle `EINTR`, and handle
  `EAGAIN`/`EWOULDBLOCK` correctly when using nonblocking sockets.
- Prevent a disconnected peer from terminating the moderator through
  `SIGPIPE`; use a deliberate Linux-compatible policy such as `MSG_NOSIGNAL`.
- For concurrent play, prefer a small `poll()`-based nonblocking implementation
  with per-connection input/output state. Readiness does not guarantee that a
  whole frame is available. A stalled player must not block everyone else.
- Implement unicast and group delivery through the same framing path. Report
  failures per recipient; sending to a group is not an atomic operation.
- Transport success is not proof that a command was accepted by the game.
  Keep game acknowledgments distinct from send completion.
- Return errors from library functions. Do not call `exit()` or decide match
  outcomes inside the communications library.

## Identity, privacy, and robustness

- Authenticate local peers using Linux `SO_PEERCRED`; use the kernel-provided
  UID for identity rather than trusting a nickname or message field.
- Allow at most one active player connection per admitted UID.
- Use a moderator-owned socket directory with group access for players.
  Players need connection permission, not permission to replace the endpoint.
- Do not run the moderator or players as root. Account/group provisioning is
  separate from running the game.
- Do not blindly unlink an existing socket on startup. Distinguish an active
  endpoint from a stale owned endpoint before cleanup.
- Send secrets only to authorized recipients. Keep roles and private actions
  out of public logs and broadcast payloads.
- Validate lengths, command types, targets, roles, and phases on the server.
  Bound queues and message sizes; malformed input must not crash the match.
- Treat chat as data: never execute it. Strip terminal control sequences before
  displaying untrusted text.
- Define disconnect and deadline behavior explicitly. Keep state changes
  serialized so concurrent submissions cannot produce inconsistent results.

## C and build conventions

- Favor readable functions, explicit ownership, and straightforward control
  flow. Avoid unnecessary dependencies, frameworks, or abstractions.
- Initialize structures, check relevant return values, close owned descriptors,
  and make buffer lengths explicit. Do not assume received bytes end in NUL.
- Use comments to explain protocol or OS behavior, not restate each line.
- Use `CC = gcc` and `CFLAGS = -Wall -Wextra -std=c11 -g` as the baseline.
- Do not add `CPPFLAGS`, `LDFLAGS`, or `LDLIBS` variables unless Riley requests
  them. Add any necessary flags through the existing simple build approach.
- Link `src/comm.c` into both executables once they use it. Headers are build
  prerequisites, not separate compilation inputs.
- Keep `make`, `make server`, `make client`, and `make clean` working. Preserve
  tab-indented recipes. `clean` removes build outputs, not live socket paths.

## Working Rules

- Match the requested scope. For an explanation, explain; for a specific file
  or function, implement that piece. Do not generate the entire game unless
  asked to do so.
- Explain new socket calls, flags, ownership decisions, and message formats in
  plain language. I need to understand the library for the assignment.
- Prefer small, reviewable changes with a clear next milestone. When a design
  choice materially affects the game, explain its tradeoff before expanding
  the implementation.
- Inspect the actual code before editing. Preserve existing work and naming
  where practical; avoid unrelated refactors.
- Never claim tests passed, feedback was received, or gameplay happened unless
  that evidence actually exists.

## Verification and final evidence

Build with the Makefile after meaningful C changes and fix new warnings.
Use focused checks appropriate to the change: connection setup, message
round-trips, partial/coalesced frames, recipient isolation, simultaneous
commands, malformed input, and abrupt disconnects. Check that a slow player
does not stall healthy players. Verify final play under distinct Linux UIDs.
Do not add a large test framework for a small change.

The final README must highlight `src/comm.c`, document the public API, explain
all submitted files, and provide reproducible setup/build/play instructions.
Verify the final archive by extracting it into a clean directory and following
those instructions.

The PDF report must include Riley's name at the top of page one, an introduction
with the library filename/API, captioned real gameplay screenshots, an
explanation of the primitive, substantive feedback received, and a closing
paragraph about what was learned, including group delivery. End with a
separately labeled section describing feedback given to other students.

Peer feedback is half the grade: 25 points for feedback received and 25 for
feedback given. Plan a Canvas review request and concrete crash/security
scenarios. Aim for roughly twice as much review effort given as received.
Keep actual findings, changes, and retest outcomes; never invent classmates,
comments, screenshots, or lessons learned. Do not post or send messages to
classmates unless Riley explicitly requests that action.
