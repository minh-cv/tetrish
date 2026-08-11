# tetriSH

A networked Tetris system written from scratch in C: a shell, a game daemon, a
logger daemon, an admin CLI and a terminal client, over a hand-rolled secure
session and a hand-rolled application protocol.

This file describes what exists today. The design documents under `docs/`
describe why each part is shaped the way it is, and the assignment spec at
`pa/tetrish` is the authority on the wire format and on which behaviours are
required rather than chosen.

## Build

```sh
git submodule update --init --recursive
cmake -B build -S . [-DTETRISH_DEV_MODE=ON]
cmake --build build
cmake --install build      # binaries to bin/, static libraries to lib/
```

`TETRISH_DEV_MODE=ON` also turns on `TETRISH_TETRISD_NO_DAEMON` and
`TETRISH_TETRISLOGD_NO_DAEMON`, which keep the two daemons in the foreground
with their stdio attached instead of detaching them. Both options are cached,
so a build tree configured without them keeps daemonizing until they are set
explicitly.

## Running

`PROJECT_DIR` must be set before any binary starts; everything else is read
from `$PROJECT_DIR/.tetrishrc`. See `sample.tetrishrc` for every directive and
its default.

```sh
export PROJECT_DIR=$PWD
mkdir -p var/log var/socket
bin/tetrislogd &         # start first, so the daemon's startup lines are kept
bin/tetrisd &
bin/tetrisu              # one per player
bin/tetrisctl status
```

From inside `bin/tetrish` the same commands work, with `.tetrishrc` in the
current directory executed as a startup script.

In `tetrisu`, `help` lists the commands. `singleplayer` creates a room, starts
it, and switches the terminal into the full-screen game; `q` leaves the room
and returns to the prompt.

## The pieces

| Binary | Role |
| --- | --- |
| `tetrish` | interactive shell: REPL, builtins, `.tetrishrc` startup script |
| `tetrisd` | the game server: TCP, secure session, rooms, authoritative game loop |
| `tetrislogd` | log collector: receives records over a Unix socket, writes them to disk |
| `tetrisctl` | admin CLI: `status`, `drain`, `shutdown` over a local control socket |
| `tetrisu` | terminal client: shell mode for commands, game mode for play |

| Library | Role |
| --- | --- |
| `tetrissh` (corestack) | certificate auth, RSA-wrapped AES, 4-byte length framing |
| `htttp` (corestack) | HTTTP parser and serializer |
| `tetrisbrain` | Tetris rules: pieces, rotation, gravity, lock delay, line clear, scoring |
| `cmdline` | the command-line tokenizer `tetrisu` and `tetrish` share |
| corestack's `network`, `server`, `config`, `logger`, `daemon`, `tui`, `tuiui` | the shared systems core |

## Architecture

`tetrisd` is a single-threaded epoll event loop. Every tick runs the same chain
of layers in a fixed order:

```
poll -> accept -> read -> handshake/decrypt -> parse
     -> app respond -> app tick (game frames) -> app flush
     -> serialize -> encrypt -> write -> sync interest -> close -> resets
```

Each layer owns per-fd state in a sparse set keyed by fd number and hands the
next layer a `{fd: [object]}` collection. There are no locks anywhere in the
daemon because there is exactly one mutator; the concurrency the spec asks
about lives in the I/O multiplexing and in the separate processes, not in
threads sharing game state. `docs/tetrisd/layers.md` states the ownership and
error-handling rules the layers follow.

Rooms are advanced by one process-wide `timerfd` at `tick_hz`, not by a timer
per room: room count is bounded by `max_rooms` but fd count is bounded by
`max_fds`, and per-room timers would spend the latter to express the former.

`tetrisu` mirrors that structure with the sparse sets removed, since it has one
connection. It adds two event sources — the terminal and a frame timer — and
two frontends over one view model. See `docs/tetrisu/layers.md`.

## Protocol

Every byte after the handshake travels as `[4-byte big-endian length][AES
ciphertext]` carrying one HTTTP message.

Methods the spec fixes:

| Method | Path | Body |
| --- | --- | --- |
| `JOIN` | `/room/<id>` | — |
| `LEAVE` | `/room/<id>` | — |
| `START` | `/room/<id>` | — |
| `MOVE` | `/room/<id>/player/<pid>` | `LEFT` or `RIGHT` |
| `ROTATE` | `/room/<id>/player/<pid>` | `CW` or `CCW` |
| `DROP` | `/room/<id>/player/<pid>` | `SOFT` or `HARD` |
| `STATE` | `/room/<id>` | server-originated snapshot |

Methods added by this implementation, because the spec provides no way to carry
a display name and no way to reach the hold piece `tetrisbrain` implements:

| Method | Path | Body |
| --- | --- | --- |
| `SET_PLAYER_NAME` | `/player` | the name |
| `WHOAMI` | `/player` | — |
| `HOLD` | `/room/<id>/player/<pid>` | — |

`Player-Id` is sent on every request and on every response. A client that has
not yet learned its id sends `-1`, which the server accepts; any other id that
is not the sender's own is a `403`. A player's id is its fd number on the
server, which is why it is stable for exactly as long as the connection is.

`STATE` is the only message the server originates, and it is sent as an HTTTP
*request* rather than a response, so the client can keep pairing its own
responses with the requests it issued. Its body is line-oriented text:

```
room=<code>            status=lobby|running|ended
seats=<n>              host=<player-id>
seat=<idx> <player-id> <name> <lines> <score> <waiting|alive|dead>
frame=<n>              you=<seat>
piece=<type> <dir> <row> <col>
ghost=<row> <col>      hold=<type|->      next=<types>
combo=<n> b2b=<n> garbage=<n> last_clear=<n>
board=<rows> <cols>
<rows lines of one character per cell: IJLOSTZ, '#' garbage, '.' empty>
```

Text rather than JSON: the client parses one of these per seat per broadcast, a
fixed grammar costs less than a document model on both sides, and a body that
can be read straight off a socket dump is worth a lot while the protocol is the
thing under discussion.

## IPC choices

Both local channels are `AF_UNIX` stream sockets carrying the same 4-byte
length framing as the network path, with no encryption, because a Unix socket's
peer is already authenticated by the kernel and the filesystem.

**Log shipping** (`log_ipc`). `tetrisd` keeps a bounded ring of formatted
records and a non-blocking socket to `tetrislogd`; the game loop only ever
appends to the ring, so it can never block on the logger. When the ring is full
records are dropped and counted, and the count goes out as a record of its own
once there is room again. A `tetrislogd` that dies is reconnected to on a timer,
and the backlog survives the reconnect. `tetrislogd` counts what it could not
take and writes a summary line every 30 seconds.

**Control plane** (`ctl_ipc`). The socket is `chmod 0600` and every peer's uid
is checked with `SO_PEERCRED`. A stale socket file is replaced only after a
connect probe proves nothing is listening, so a second daemon cannot steal a
running daemon's control channel.

Staying reachable while the public listener is saturated is the requirement
that shapes it. Three things together: the control listener is registered in
the same epoll so an arriving command wakes an idle daemon; the tick drains
that listener directly at its top rather than reading it out of the event
array, so an array full of player fds cannot delay it; and `max_ctl_fds` slots
are held back from player admission so the control accept always has an fd. A
control connection is then served to completion synchronously, which is only
safe because the peer is local, privileged, and on a short timeout.

## Configuration

Every path in `.tetrishrc` is resolved against `PROJECT_DIR` unless it is
absolute. `sample.tetrishrc` lists all directives. Two are worth calling out:
`log_ipc` and `ctl_ipc` are Unix socket paths and inherit the ~108 byte
`sun_path` limit, so a project directory nested deeply enough will need them
pointed at a shorter absolute path.

## Known limitations

* Battle royale is not implemented. Garbage is routed between seats inside one
  room; there is no cross-room transfer and no cross-room IPC yet.
* Host enforcement exists for `START` only. Host transfer happens implicitly
  when the host leaves, and there is no way to hand it over deliberately.
* The client has no reconnect. `tetrish_client_handshake` blocks, so the
  handshake runs once at startup; a lost connection ends the session.
* `tui` has no resize handling, so a terminal resized during a game keeps
  drawing at the size it started with until the game mode is left and re-entered.
* `tetrisd` has no `SIGHUP` config reload and no `SIGUSR1` state dump yet;
  `tetrislogd` does handle `SIGHUP`.
* There is no automated test suite. The layers were exercised by hand against a
  running daemon, and `tetrisd`, `tetrisu`, `tetrisctl` and `tetrish` each run a
  full session under valgrind with no leaks and no errors; `world` and `game`
  were split out of the layer shell specifically so they can be driven without
  sockets when tests are written.
