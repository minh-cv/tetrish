# tetrish

A C systems-programming project: a custom shell (`tetrish`) plus a set of supporting daemons, libraries, and permission-aware system utilities. Commit history indicates this began as a course assignment ("pa1 implementation of shell"); several components are still stubs.

## Components

Binaries (`src/*/main.c`):
- `tetrish` — the shell itself (`src/tetrish/main.c`, `src/tetrish/shell.c`). Reads a command line, resolves builtins, forks/execs. Config via `.tetrishrc` (see `sample.tetrishrc` for format: `PATH=...`, `setenv KEY=value`).
- `tetrisd` — daemon; **currently a stub** (`int main() { return 0; }`).
- `tetrisu` — currently a stub.
- `tetrisctl` — control utility for `tetrisd`.
- `tetrislogd` — logging daemon.

System programs (`src/tetrish/system_programs/`), linked against the `perms` lib:
- `ld`, `ldr`, `find` — permission-aware directory/file tools.
- `backup` — creates an `archive/` directory and backs up into it.
- `sys` — system info/utility program.

Libraries:
- `libtetrissh` (`tetrissh.h`) — **stub only** (`dummy()`).
- `libhtttp` (`htttp.h`) — **stub only** (`dummy()`). Presumably an HTTP-like protocol layer, not yet implemented.
- `libtetrisbrain` (`tetrisbrain.h`) — **stub only** (`dummy()`).
- `common` (`common.c`/`common.h`) — OpenSSL-backed crypto/socket helpers (RSA, AES-128-CBC+HMAC "Fernet-equivalent", X.509 cert verification). Note: this header currently self-describes as being for a "Secure FTP project," not `tetrish` — unclear if intentionally reused or stale; see `.claude/rules/clang-design.md` for the open question.
- `perms` — permission string formatting (`perms_to_string`).

`auth/` holds cert/key material for the crypto layer (`generate_keys.sh`, `cacsertificate.crt`).

Most of the actual logic lives in `libtetrissh`, `libhtttp`, and `libtetrisbrain` per their names, but as of now those are all unimplemented stubs — treat this repo as early-stage scaffolding, not a finished system.

## Build

CMake + make:
```
cmake -B build && cmake --build build
```
Compiled with `-Wall -Wextra -Wpedantic -Wconversion` — treat new warnings as bugs to fix, not to silence.

Doxygen docs (if `doxygen` is installed): `cmake --build build --target docs` → output in `docs/doxygen/html`.

No test suite exists yet — see `.claude/rules/testing.md` before writing or assuming tests.

## Conventions

C coding conventions (naming, error handling, what's consistent vs. not yet decided in this codebase) are documented in `.claude/rules/clang-design.md` — read it before making style judgment calls; it explicitly separates "established convention" from "exists but unused" (e.g. `dtor.h`'s cleanup macros) from "inconsistent, needs a decision" (header guard naming, `common.h`'s mismatched project description).
