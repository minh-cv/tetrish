# tetrisu/tetrisd reference stack

This branch contains a runnable reference path from the raw-mode `tetrisu`
reactor through `tetrissh`, the `tetrisd` application layer, periodic server
push, and `tetrislogd`.

## Run it

The helper creates ignored development credentials and `.tetrishrc`, builds
the three processes, starts the two servers in foreground mode, and restores
them when the TUI exits:

```sh
scripts/run-server-ref.sh
```

The generated private key and local configuration stay below ignored paths and
must not be committed. Existing `.tetrishrc` files are never overwritten.

Inside tetrisu:

```text
set-name Alice
whoami
htttp hello
```

`STATE` snapshots appear automatically in the upper TUI panel. Their reference
JSON contains a monotonic sequence, the current fd-backed player id and name,
and the authenticated player count.

## Protocol implemented by the reference application

| Direction | Method/status | Body |
|---|---|---|
| client to server | `SET_PLAYER_NAME` | 1-32 safe ASCII name bytes |
| client to server | `WHOAMI` | empty |
| client to server | `HTTTP` | arbitrary debug text |
| server to client | HTTTP response | identity JSON, debug echo, or error text |
| server to client | `STATE` request | application state JSON |

The client permits one request in flight. A `STATE` request is an independent
one-way push and never completes that request. An output-idle client receives at
most one new STATE per timer pass. STATE is best effort under backpressure;
responses are not silently dropped—a client unable to queue a response is
closed.

## Test it

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The process-level test generates an isolated CA and server certificate, starts
real `tetrislogd` and `tetrisd` processes on temporary endpoints, completes the
incremental handshake, sets a player name, queries it, observes a subsequent
STATE push, and verifies the IPC log record.

`tetrisd` treats logging as optional for availability: it can start before
`tetrislogd`, periodically retries the Unix-socket connection, probes a live
logger connection, and reconnects after a detected send failure. Shutdown
closes the logger descriptor only when ownership was established.

This is deliberately a reference application, not the final room/game engine.
The player id remains the connection fd, and STATE contains metadata rather
than a `tetrisbrain` board.
