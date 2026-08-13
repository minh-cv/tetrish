# tetrish

A C systems-programming project: a networked multiplayer Tetris stack — an
epoll game server, a terminal client, control and logging daemons — speaking
an encrypted, HTTP-like protocol, plus the custom shell and Unix utilities
the project began with as a course assignment.

A player's command travels `tetrisu` → `libproto`/`libhtttp` → `libtetrissh` →
TCP → `tetrisd` → `libtetrisbrain`, and the resulting `STATE` snapshot returns
along the same path. The server is authoritative: the client renders state and
collects keystrokes, but never advances a board of its own.

The transport, message, config, logging, and terminal-UI primitives live in the
[corestack](https://github.com/minh-cv/corestack) submodule
(`external/corestack`); this repository builds the applications and game
logic on top of it.

## Build

Requires CMake ≥ 3.10, a C compiler, OpenSSL (`libcrypto`), and pthreads. A
`.devcontainer` is provided.

```sh
git submodule update --init          # corestack + cJSON
cmake -B build
cmake --build build
```

Everything compiles with `-Wall -Wextra -Wpedantic -Wconversion`; treat new
warnings as bugs to fix, not to silence.

Options:

- `-DTETRISH_DEV_MODE=ON` — dev mode; currently implies the two below.
- `-DTETRISH_TETRISD_NO_DAEMON=ON` / `-DTETRISH_TETRISLOGD_NO_DAEMON=ON` —
  keep the daemons in the foreground (required for the system test).

## Quickstart

### `.tetrishrc`

Every path in the system comes from `.tetrishrc` and is resolved relative to
the project root; nothing is hard-coded. Copy [`sample.tetrishrc`](sample.tetrishrc)
and edit. Shell settings come first, then daemon directives.

| Directive | Default | Meaning |
| --- | --- | --- |
| `PATH=…`, `setenv K=V` | — | shell environment, applied at startup |
| `listen_port` | *required* | `tetrisd` TCP port |
| `tetrisd_address` | `localhost` | address `tetrisu` dials |
| `cert_path`, `key_path`, `ca_path` | *required* | server certificate, server key, CA bundle |
| `log_path` | *required* | file `tetrislogd` appends to |
| `log_ipc` | *required* | Unix socket `tetrislogd` binds |
| `tetrisd_control_ipc` | `$PROJECT_DIR/tetrisd.sock` | control socket `tetrisctl` dials |
| `tetrisd_max_fds` | 1024 | descriptor table size |
| `tetrisd_max_player_fd` | 1008 | highest descriptor admitted as a player |
| `tetrisd_max_events` | 64 | `epoll_wait` batch size |
| `tetrisd_max_rooms` | 512 | concurrent rooms |
| `tetrisd_room_tick_hz` | 60 | game clock, capped at 10⁹ |
| `tetrisd_client_capacity` | 8 (min 2) | per-player queue depth; see [Backpressure](#backpressure) |
| `tetrisd_logger_capacity` | 512 | log records buffered in `tetrisd` |
| `tetrisd_logger_reconnect_seconds` | 5 | retry interval when `tetrislogd` is absent |
| `tetrislogd_max_clients` | 64 | concurrent log producers |

A directive that is absent takes its default; a directive that is present but
invalid is rejected at startup rather than silently corrected.

### Shell

```sh
bin/tetrish
```

Reads `.tetrishrc` on startup, then a REPL: builtins are resolved first,
anything else is `fork`+`execvp`. Builtins are `cd`, `help`, `exit`, `usage`,
`env`, `setenv`, `unsetenv`.

### Server, logger, and control

Start the logger first — it creates `log_ipc` — then the server:

```sh
bin/tetrislogd
bin/tetrisd
bin/tetrisctl status
```

`tetrisd` retries the logger every `tetrisd_logger_reconnect_seconds`, so the
two are not lifetime-coupled and either may restart independently.

### Client

```sh
bin/tetrisu
```

Commands: `create`, `join`, `start`, `move`, `rotate`, `drop`, `hold`, `leave`,
`reconnect`, `quit`. Generate key material first with `auth/generate_keys.sh`,
which produces the server key and a certificate signed by the bundled CA.

## Directory

```
auth/      key material and generate_keys.sh
docs/      per-component design notes
external/  corestack (transport, config, logging, tui) and cJSON
include/   public headers for this repository's libraries
specs/     the original assignment spec
src/       tetrish, tetrisd, tetrisu, tetrisctl, tetrislogd, cmdline,
           libtetrisbrain, libproto
tests/     unit and system tests
var/       runtime state: log/ and socket/ (socket/ must exist beforehand)
```

## Design decisions

Every open-design question the spec poses, and where it is answered.

| Question | Decision |
| --- | --- |
| tetrish-specific builtins | **None added** — the seven PA1 builtins only; daemon control belongs in `tetrisctl`, not in the shell |
| `tetrisd` internal architecture | Single-threaded layered epoll reactor — [tetrisd](#tetrisd) |
| `tetrisd` ↔ `tetrislogd` IPC | Unix-domain **stream** socket with reconnect — [IPC choices](#ipc-choices) |
| Log channel wire format | Length-prefixed binary frames, level-tagged — [tetrislogd](#tetrislogd) |
| Logger lifecycle | Launched independently, logger first — [Quickstart](#server-logger-and-control) |
| Internal log buffering in `tetrisd` | Bounded queue of `tetrisd_logger_capacity`, drops observably |
| Whether `tetrisu`/`tetrisctl` also log | **No** — only `tetrisd` produces records |
| `tetrisctl` commands and IPC | `status`, `shutdown`, `reload` over a Unix socket — [tetrisctl](#tetrisctl) |
| Client rendering technique | Raw ANSI via corestack's `tui` — [tetrisu](#tetrisu) |
| Client reconnection | **Supported**, as an explicit `reconnect` command |
| `libtetrissh` API shape | Deliberately *not* `session_*`; decomposed for a non-blocking server — [libtetrissh](#libtetrissh-corestack) |
| `libhtttp` parsing model | One-shot over a complete buffer, explicit ownership — [libhtttp](#libhtttp-corestack) |
| Rotation, scoring, gravity, lock delay | SRS-style kicks, 7-bag, guideline-like lock delay — [libtetrisbrain](#libtetrisbrain) |
| Additional HTTTP methods | **None** beyond the game set |
| Threads vs processes, lock granularity | Single-threaded server; two mutexes in the logger — [Concurrency](#concurrency-and-locking) |
| Battle-royale garbage IPC | POSIX message queue — **not on `main`**, lands with `feat/battle-royale` |

## Components

### Game stack

#### `tetrisd`

Epoll-based game server. Sources are layered — `socket` → `acceptor` →
`player_io`/`auth` → `htttp_layer` → `app` — and one `server_tick` runs every
layer in a fixed order over the whole descriptor set: read → decrypt → parse →
dispatch → game tick → serialise → encrypt → write.

The layering is the architectural decision. Each layer owns one transformation
and one queue, states its capacity as a precondition, and marks a descriptor
failed rather than unwinding the whole tick. Concurrency is expressed as
*pipelining across ticks* rather than threads, which is why the load-bearing
invariants are queue capacities rather than locks.

See [`docs/tetrisd/layers.md`](docs/tetrisd/layers.md).

#### `tetrisu`

Terminal client: a command REPL (`command/`), a nonblocking network stack
(`net/`: transport → tetrissh channel → HTTTP codec → events), and gameplay
rendering on corestack's `tui`, which draws with raw ANSI escape sequences and
double-buffers to avoid flicker. `ncurses` was not used — the renderer needs
only a diffed cell grid, and the dependency would outweigh it.

Input is nonblocking so network events and keystrokes interleave in one loop.
Reconnection is supported and explicit: `reconnect` re-runs the handshake and
rejoins, rather than the client silently reconnecting under the player.

See [`docs/tetrisu/gameplay.md`](docs/tetrisu/gameplay.md).

#### `tetrisctl`

Control utility. Speaks HTTTP — the format the spec fixes for this channel —
with JSON bodies (cJSON), over `tetrisd`'s Unix-domain control socket, and
reports results through distinct exit codes so it composes in scripts.

| Command | Request | Effect |
| --- | --- | --- |
| `status` | `GET /status` | snapshot of the running daemon |
| `shutdown` | `POST /shutdown` | exit once the reply is out |
| `reload` | `POST /reload` | re-read the reloadable directives |

`reload` is the one addition beyond the required pair. See
[`docs/tetrisd/control.md`](docs/tetrisd/control.md).

#### `tetrislogd`

Logging daemon. Accepts framed records over a Unix-domain socket, one detached
thread per producer, and appends them to `log_path`. Records are length-prefixed
binary frames carrying a level tag (`debug`, `info`, `warning`, `error`), reusing
`libtetrissh`'s framing rather than inventing a line format — records may contain
arbitrary bytes, and a length prefix removes any escaping question.

Producers never block: `tetrisd` buffers into a bounded queue of
`tetrisd_logger_capacity` records and drops when it fills, so overload costs
records rather than tick latency. Drops are **observable, not silent** — the
count is carried and later emitted into the log stream itself as
`dropped N records` at `warning` level, the same way `systemd-journald` reports
suppressed messages. The reporting path deliberately never logs while building
that record, since a record produced while reporting a drop could not itself be
queued.

See [`docs/tetrisd/logger.md`](docs/tetrisd/logger.md).

#### `libtetrisbrain`

The Tetris rules engine — deterministic and I/O-free, so the server can run it
authoritatively and the same code can be unit-tested off the network.

| Choice | Decision |
| --- | --- |
| Board | 10 × 40 cells, the upper half a spawn buffer |
| Randomiser | 7-bag with a second bag held for lookahead |
| Rotation | SRS-style, five wall-kick candidates per rotation state |
| T-spin | Recognised from the kick index that succeeded, not from board shape |
| Lock delay | Frame counter that resets on movement, with a bounded reset count so a piece cannot be held indefinitely |
| Gravity | Per-level frame counter; the interval shortens as the level rises |
| Scoring | Combo and back-to-back multipliers, `SCORE_PER_GARBAGE` per garbage line, saturating at `INT_MAX` |

#### `libproto`

Conversion between HTTTP message bodies and domain types. `MOVE`, `ROTATE`,
`DROP` and `HOLD` travel client→server; `STATE` is pushed server→client. See
[`include/proto.h`](include/proto.h).

Methods are `CREATE`, `JOIN`, `START`, `LEAVE`, `MOVE`, `ROTATE`, `DROP`,
`HOLD`. No `CHAT`, `PAUSE` or `SPECTATE` — the dispatcher is a flat switch on a
parsed method enum, so adding one is a case arm and a body codec.

### Libraries from corestack

#### `libhtttp` (corestack)

Parsing is **one-shot**: `htttp_parse(buffer, size, msg)` takes a complete frame
and fills a message. It can be, because `libtetrissh` already delivers whole
frames — a streaming parser would re-solve a problem the transport has solved.
The parsed message borrows from the caller's buffer, and a companion
`HtttpMessageOwnership` records what was heap-allocated, so a message can mix
borrowed and owned fields with one free path.

Parse failures travel in-band as a status rather than as an exception: a
malformed frame becomes a `400`, an oversized one a `413`, so a bad frame costs
its own request and not the connection.

#### `libtetrissh` (corestack)

The API is deliberately *not* the suggested
`session_handshake_server()`/`session_send()` shape. A blocking
`session_handshake_server()` cannot exist in a single-threaded reactor — it
would stall every other player mid-handshake. So the client side is one call,
`tetrish_client_handshake()`, while the server side is decomposed into
primitives (`tetrish_server_sign_nonce`, `tetrish_server_decrypt_session_key`)
that `tetrisd`'s `auth` layer drives from a per-connection state machine as
bytes arrive.

Framing is shared: `tetrish_send_frame`/`tetrish_recv_frame` take an optional
session key, so the same length-prefixed framing serves the encrypted TCP
channel and the plaintext log socket.

### Shell and system programs

| Binary / library | What it does |
| --- | --- |
| `tetrish` | The shell (`src/tetrish/`). Reads a command line, resolves builtins, forks/execs. |
| `cmdline` | POSIX-style tokenizer (quoting, `$'…'` escapes, line continuation) producing an execvp-ready argv. |
| `perms` | Permission-string formatting used by the file utilities. |
| `ld`, `ldr`, `find` | Permission-aware directory listing and search. |
| `backup`, `sys` | Backup into `archive/`; system info. |
| `dspawn`, `dcheck`, `directive` | Daemon spawn/check demos and a directive helper. |

## Concurrency and locking

`tetrisd`, `tetrisu` and `tetrisctl` are **single-threaded by design**. Because
`tetrisd` runs one `server_tick` over the whole descriptor set, shared state has
exactly one writer at any instant and needs no mutex at all.

`tetrislogd` is the only threaded component: one detached thread per connected
client, plus the accept loop on the main thread. Its locking strategy is two
mutexes with disjoint scopes:

| Mutex | Protects | Acquired by |
| --- | --- | --- |
| `LogdCtx.out_file_mu` | `out_file` — the `FILE*` and its buffer | client threads appending a record; the main thread swapping the log file on `SIGHUP` |
| `LogdCtx.clients_mu` | `client_count`, `max_clients` | client threads on exit; the main thread on accept, on rejection, and in the shutdown drain |

**Lock acquisition order: no thread ever holds one of these while acquiring the
other.** Every critical section takes exactly one mutex and releases it before
taking another — including the `SIGHUP` path, which finishes the `out_file_mu`
section before touching `clients_mu`. The order is therefore total by
construction and deadlock is unreachable rather than merely avoided.

`incantation()` daemonizes before any thread is created, so the
fork-from-a-threaded-process rule never applies: the only `fork` happens while
the process is still single-threaded.

**Known deviation.** `fwrite` and `fflush` run while `out_file_mu` is held, so a
blocking write does happen under a lock. It bounds contention to the log file
rather than the socket, and the alternative — per-thread staging buffers drained
by a single writer thread — was judged more machinery than a local append
warrants. It is a deliberate trade, not an oversight.

### Backpressure

A player's write queue is sized at twice the read capacity, and `PlayerIo_write`
reports `WRITER_QUEUE_FULL` once it passes half. `Epoll_sync_interest` then
withholds `EPOLLIN` for that descriptor, so no further request is admitted. The
reserved half covers everything one tick can still deliver — at most
`tetrisd_client_capacity` frames, since `response_qs` and `encrypt_qs` are each
sized that way and every stage emits at most one frame per input frame. The
consequence is that **a response is never dropped**: a server-pushed `STATE`
frame competes for the same `response_qs` slots and, being enqueued after the
responses, is the one discarded under pressure. `AuthData_encrypt` asserts the
queue is never full at that point.

## IPC choices

| Channel | Mechanism | Wire format |
| --- | --- | --- |
| `tetrisu` ↔ `tetrisd` | TCP (`listen_port`) | `tetrissh` frames — RSA-OAEP-wrapped AES-128-CBC session key with HMAC — carrying HTTTP |
| `tetrisctl` ↔ `tetrisd` | Unix-domain socket, `tetrisd_control_ipc` | HTTTP envelope with JSON bodies |
| `tetrisd` → `tetrislogd` | Unix-domain socket, `log_ipc` | length-prefixed frames (`tetrish_send_frame` with a null session key, i.e. framed but not encrypted) |

The control plane is a separate Unix-domain socket rather than a command on the
public TCP port, so it stays reachable when the listener is saturated or has
stopped accepting — saturation of the game port cannot lock an operator out of
`status` or `shutdown`.

The log channel is a **stream socket rather than a message queue**: records are
variable-length, ordering per producer matters, and a stream socket reconnects
cleanly when `tetrislogd` restarts, which a FIFO cannot. The cost is that the
reconnect logic lives on the `tetrisd` side — `tetrisd_logger_reconnect_seconds`
— and that the bound on buffering is ours to enforce rather than the kernel's,
which is what `tetrisd_logger_capacity` does. Encryption is omitted because the
channel never leaves the host and the socket's `0600` mode is the boundary.

## Security assumptions

- **The transport is authenticated in one direction only.** A client verifies
  the server's X.509 certificate against the bundled CA and proves liveness with
  a nonce; the server does **not** verify a client certificate. Any peer that
  completes the handshake is accepted as a player.
- **There is consequently no client identity**, and therefore no authorization
  model: the daemon cannot distinguish one authenticated player from another.
- **Local IPC sockets are protected by file mode, not by credentials.** Both
  `log_ipc` and `tetrisd_control_ipc` are created `0600` under a scoped `umask`,
  which is necessary because `incantation()` sets `umask(0)`; that mode is the
  entire auth boundary for the control and log planes.
- **Room seeds come from OpenSSL's CSPRNG** (`RAND_bytes`) and are never logged.
  A seed fixes the whole piece sequence and every garbage hole column, and
  `shared_seed` hands the same one to both players, so a guessable or disclosed
  seed would be a competitive break rather than a cosmetic one.
- **Configuration is trusted.** Paths in `.tetrishrc` are taken as given,
  resolved relative to the project root, and are assumed writable only by the
  operator.

## Testing

```sh
cmake -B build && cmake --build build
ctest --test-dir build
```

- `tetrisu_unit_tests` always builds under `BUILD_TESTING`.
- `tetrisu_gameplay_system` — an end-to-end probe that boots `tetrisd` and
  `tetrislogd` and plays through the network stack — is added only when both
  `*_NO_DAEMON` options are on (e.g. configure with `-DTETRISH_DEV_MODE=ON`)
  and requires the `openssl` CLI.

## Documentation

- [`docs/`](docs) — design notes per daemon/client (`tetrisd` layers, control
  protocol, logger; `tetrisu` gameplay).
- [`specs/tetrish-requirements.html`](specs) — the original assignment spec.
- `cmake --build build --target docs` — Doxygen API docs (if installed) into
  `docs/doxygen/html`.
- Corestack's own [README](external/corestack/README.md) documents the
  transport framing, handshake, and library APIs.

## Conventions

C conventions live in `.claude/rules/clang-design.md`. In brief: `snake_case`
with module prefixes, `TETRISH_<MODULE>_<NAME>_H` header guards, and the
`dtor.h` destructor-stack cleanup pattern (register cleanup as soon as a
resource exists; leave through `DTOR_RETURN`, never a bare `return`).

## Known limitations

### Operational

* On unrealistically low `max_fds`, `accept` might not be re-armed. The listener
  is re-armed only when a player disconnects, so if it is disarmed while no
  player is connected nothing can re-arm it. It is only a problem if `max_fds`
  is so low that a player can't be accepted, which is absurd for a server.
* `tetrisd` and `tetrislogd` failure is literally untraceable under daemon form:
  there is no logging on the paths that fail before the logger is up.
* `tetrislogd` requires making directory `var/socket` beforehand, otherwise the
  syscall will fail.
* `tetrislogd` closing behavior is more like a band-aid rather than a proper
  solution: the shutdown drain waits a bounded five seconds and then `_exit`s,
  which skips the socket unlink. A stale socket file is removed on next start,
  so it self-heals.
* Neither daemon applies a handshake or idle timeout. A connection that
  completes the TCP accept but never finishes the `tetrissh` handshake holds its
  descriptor and player slot indefinitely, so an unauthenticated peer can occupy
  every slot at the cost of one socket each.

### Protocol and gameplay

* No good recovery behavior on discarding `STATE` message. If the `STATE`
  message signifying losing is dropped, client has no good way to deal with it.
  Also an architectural problem, because the server cannot guarantee that any
  event-like `STATE` will be transmitted — the wire format carries no winner or
  placement field, so a dropped final snapshot loses the result entirely.
* (Relating to previous one) Current backpressuring is the easiest but not the
  best solution. A more permissive server should allow movement even if write
  buffer is full.
* The client treats any unsolicited server frame as fatal. Gameplay intents are
  sent fire-and-forget, so a frame-level error response — which the server emits
  unconditionally for a malformed or truncated frame — is classified as
  unsolicited and tears down the session rather than costing one input.
* `apply_level_tick` zeroes the gravity counter on every level-up, working
  around a comparison that tests `!=` rather than `>=`. It is latent rather than
  live on `main`, where level pacing is fixed; it becomes a real starvation bug
  the moment pacing is client-settable, since a level interval shorter than the
  gravity period would keep resetting the counter before it can fire.

### Rooms and multiplayer

* **Rooms hold exactly one member on this branch**, and `METHOD_JOIN` is routed
  to the same handler as `METHOD_CREATE`, which allocates a fresh room rather
  than seating the player in an existing one. A player therefore cannot enter
  another player's room on `main`; shared rooms — and with them any
  authorization surface, since there is no client identity to authorize against
  — arrive with `feat/battle-royale`.
