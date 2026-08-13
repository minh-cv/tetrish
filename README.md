# tetrish

A C systems-programming project: a networked multiplayer Tetris stack — an
epoll game server, a terminal client, control and logging daemons — speaking
an encrypted, HTTP-like protocol, plus the custom shell and Unix utilities
the project began with as a course assignment.

The transport, message, config, logging, and terminal-UI primitives live in the
[corestack](https://github.com/minh-cv/corestack) submodule
(`external/corestack`); this repository builds the applications and game
logic on top of it.

## Building

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

## Components

### Game stack

| Binary / library | What it does |
| --- | --- |
| `tetrisd` | Epoll-based game server. Authenticates clients with the `tetrissh` handshake, speaks `HTTTP/1.0` over encrypted frames, runs multiplayer rooms on a fixed tick (`tetrisd_room_tick_hz`), exposes a Unix-domain control socket, and forwards logs to `tetrislogd`. Sources are layered (`socket` → `acceptor` → `player_io`/`auth` → `htttp_layer` → `app`); see [`docs/tetrisd/layers.md`](docs/tetrisd/layers.md). |
| `tetrisu` | Terminal client. A command REPL (`command/`), a nonblocking network stack (`net/`: transport → tetrissh channel → HTTTP codec → events), and gameplay rendering on corestack's `tui`. See [`docs/tetrisu/gameplay.md`](docs/tetrisu/gameplay.md). |
| `tetrisctl` | Control utility. Sends JSON requests (cJSON) to `tetrisd`'s control socket and reports the result through distinct exit codes. See [`docs/tetrisd/control.md`](docs/tetrisd/control.md). |
| `tetrislogd` | Logging daemon. Accepts framed log records over a Unix-domain socket, one thread per client, and appends them to the log file. |
| `libtetrisbrain` | The Tetris rules engine: board and tetromino state, rotation, ghost piece, hold, lock delay, line clears, garbage, scoring, and seeded RNG. Deterministic and I/O-free, so the server can run it authoritatively. |
| `libproto` | Conversion between HTTTP message bodies and domain types. `MOVE`, `ROTATE`, and `DROP` requests travel client→server; `STATE` is pushed server→client. See [`include/proto.h`](include/proto.h). |

### Shell and system programs

| Binary / library | What it does |
| --- | --- |
| `tetrish` | The shell (`src/tetrish/`). Reads a command line, resolves builtins, forks/execs. Configured by `.tetrishrc` (`PATH=...`, `setenv KEY=value` — see [`sample.tetrishrc`](sample.tetrishrc)). |
| `cmdline` | POSIX-style command-line tokenizer (quoting, `$'...'` escapes, line continuation) producing an execvp-ready argv. |
| `perms` | Permission-string formatting library used by the file utilities. |
| `ld`, `ldr`, `find` | Permission-aware directory/file listing and search. |
| `backup`, `sys` | Backup into an `archive/` directory; system info. |
| `dspawn`, `dcheck`, `directive` | Daemon spawn/check demos and a directive helper. |

## Running

1. Generate key material: `auth/generate_keys.sh` produces the server key and
   certificate signed by the bundled CA (`cacsertificate.crt`).
2. Write a `.tetrishrc` from [`sample.tetrishrc`](sample.tetrishrc): shell
   settings at the top, then daemon directives (`listen_port`, `cert_path`,
   `key_path`, `ca_path`, `log_path`, `log_ipc`, and the optional
   `tetrisd_*`/`tetrislogd_*` tunables).
3. Start `tetrislogd`, then `tetrisd`, then connect with `tetrisu`. Inspect or
   control the server with `tetrisctl`.

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
