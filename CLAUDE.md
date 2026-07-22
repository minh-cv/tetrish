@AGENTS.md
# tetrish

A C systems-programming project: a custom shell (`tetrish`) plus daemons, libraries, and permission-aware system utilities that communicate over an encrypted HTTP-like protocol. Began as a course assignment ("pa1 implementation of shell").

**Component status varies by branch** — the stub/implemented notes below were last verified 2026-07-22 on `feat/sample-implementation`. Check the actual sources before assuming; a `main()` containing only `return 0;` or a header exposing only `dummy()` means stub.

## Components

Binaries (`src/*/`):
- `tetrish` — the shell (`main.c`, `shell.c`). Reads a command line, resolves builtins, forks/execs. Config via `.tetrishrc` (see `sample.tetrishrc`: `PATH=...`, `setenv KEY=value`). Internal headers in `src/tetrish/libs/`.
- `tetrisd` — epoll-based daemon (`client.c`, `state.c`, `epollmanip.c`, `config_var.c`). Accepts framed, encrypted connections.
- `tetrisu` — client utility; sends `htttp` messages over encrypted frames (`tetrissh`) to the daemon.
- `tetrisctl` — control utility for `tetrisd`; **stub**.
- `tetrislogd` — logging daemon; **stub**.

System programs (`src/tetrish/system_programs/`), mostly linked against the `perms` lib:
- `ld`, `ldr`, `find` — permission-aware directory/file tools.
- `backup` — backs up into an `archive/` directory.
- `sys` — system info program.
- `hash` — installed with output name `#`.

Libraries:
- `libhtttp` (`include/htttp.h`) — HTTP-like message layer: parse/serialize requests and responses with headers and body.
- `libtetrissh` (`include/tetrissh.h`) — secure transport: session-key handshake and encrypted framing over sockets (uses `common` + OpenSSL).
- `config` (`include/config.h`) — `Config` struct with required/optional directives; used by `tetrisd` and `tetrisu` (each has its own `config_var.c`).
- `common` (`include/common.h`) — OpenSSL-backed crypto/socket helpers (RSA, AES-128-CBC+HMAC "Fernet-equivalent", X.509 verification). Header comment still says "Secure FTP project" — reused code, stale description (open question in `.claude/rules/clang-design.md`).
- `perms` (`src/tetrish/libs/`) — permission string formatting.
- `libtetrisbrain` (`include/tetrisbrain.h`) — **stub only** (`dummy()`).
- `dtor.h` (`include/`, header-only) — scope-based cleanup macros; the established error-path cleanup pattern. See `.claude/rules/clang-design.md` for usage rules before writing resource-owning code.

`auth/` holds cert/key material (`generate_keys.sh`, `cacsertificate.crt`).

## Build

CMake + make (requires OpenSSL; a `.devcontainer` is provided):
```
cmake -B build && cmake --build build
```
Compiled with `-Wall -Wextra -Wpedantic -Wconversion` — treat new warnings as bugs to fix, not to silence.

Libraries build static (default, not `SHARED`). Public headers live in `include/`; module-private headers next to their sources.

Doxygen docs (if installed): `cmake --build build --target docs` → `docs/doxygen/html`.

No test suite exists yet — see `.claude/rules/testing.md` before writing or assuming tests.

## Conventions

C coding conventions are in `.claude/rules/clang-design.md` — read it before making style judgment calls. Non-negotiables in brief: `TETRISH_<MODULE>_<NAME>_H` header guards, `snake_case` with module prefixes, and the `dtor.h` cleanup pattern (`DTOR_RETURN`, never bare `return`, after resources are registered). The rule file also tracks what's still inconsistent or undecided.
