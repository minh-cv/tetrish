# tetrish

A C systems-programming project centered on a custom shell (`tetrish`) plus a
client/server pair — **`tetrisu`** (client) and **`tetrisd`** (server/daemon)
— that talk to each other over an encrypted, length-framed, HTTP-like
protocol. A handful of permission-aware system utilities (`ld`, `ldr`, `find`,
`backup`, `sys`, `hash`) round out the shell side of the project.

Component status varies by branch. This document reflects `feat/sample-implementation`
as of 2026-07-22. A `main()` containing only `return 0;` or a header exposing
only `dummy()` means stub — see [Implementation status](#implementation-status).

## Contents

- [Build](#build)
- [Client/Server: technical overview](#clientserver-technical-overview)
- [Running the server (`tetrisd`)](#running-the-server-tetrisd)
- [Running the client (`tetrisu`)](#running-the-client-tetrisu)
- [Wire protocol](#wire-protocol)
- [The shell (`tetrish`) and system programs](#the-shell-tetrish-and-system-programs)
- [Implementation status](#implementation-status)

## Build

Requires CMake ≥ 3.10, a C compiler, and OpenSSL development headers
(`libssl-dev` on Debian/Ubuntu). A ready-made dev container is provided under
`.devcontainer/` (Ubuntu 22.04 + build-essential + clang + cmake + gdb).

```sh
cmake -B build && cmake --build build
```

This builds, among others:

- Libraries (static): `common`, `libhtttp`, `libtetrissh`, `config`, `perms`, `libtetrisbrain`
- Binaries: `tetrisd`, `tetrisu`, `tetrisctl`, `tetrislogd`, `tetrish`, `ld`, `ldr`, `find`, `backup`, `sys`, `hash`

Build flags are `-Wall -Wextra -Wpedantic -Wconversion` (new warnings are
treated as bugs). Optional Doxygen docs: `cmake --build build --target docs`
→ `docs/doxygen/html`.

No automated test suite exists yet (see `.claude/rules/testing.md`) — verify
behavior manually against the built binaries.

## Client/Server: technical overview

`tetrisd` and `tetrisu` are the two halves of the same protocol stack:

| | `tetrisd` (server) | `tetrisu` (client) |
|---|---|---|
| Role | epoll-based daemon, accepts many concurrent connections | interactive CLI, one connection at a time |
| Identity | holds an RSA private key + X.509 certificate, proves its identity to clients | verifies the server's certificate against a trusted CA |
| Source | `src/tetrisd/{main,client,state,epollmanip,config_var}.c` | `src/tetrisu/{main,config_var}.c` |
| Depends on | `libhtttp`, `common`, `libtetrissh`, `config` | same |

Both link against three shared libraries:

- **`common`** (`include/common.h`) — OpenSSL wrappers: socket read/write
  helpers, RSA sign/verify/encrypt/decrypt, X.509 cert loading/verification,
  and a Fernet-equivalent symmetric cipher (AES-128-CBC + HMAC-SHA256).
- **`libtetrissh`** (`include/tetrissh.h`) — the secure transport built on
  top of `common`: the handshake (`tetrish_client_handshake`,
  `tetrish_server_sign_nonce`, `tetrish_server_decrypt_session_key`) and
  encrypted frame I/O (`tetrish_send_frame` / `tetrish_recv_frame`).
- **`libhtttp`** (`include/htttp.h`) — an HTTP-like message format (method,
  path, status, headers, body) that is serialized/parsed and sent as the
  *plaintext* payload inside each encrypted frame.

`config` (`include/config.h`) is a small directive-reader that both
`tetrisd` and `tetrisu` use (each has its own `config_var.c` describing its
own directives) — see below for exactly what it reads.

## Running the server (`tetrisd`)

### Configuration

`tetrisd` reads configuration **from environment variables**, not a config
file (`src/tetrisd/config_var.c`). `PROJECT_DIR` must always be set — relative
`cert_path`/`key_path` values are resolved against it.

| Variable | Required | Default | Meaning |
|---|---|---|---|
| `PROJECT_DIR` | yes | — | base directory for relative paths below |
| `cert_path` | yes | — | path to the server's X.509 certificate (PEM) |
| `key_path` | yes | — | path to the server's RSA private key (PEM) |
| `listen_port` | no | `4321` | TCP port to listen on |
| `address` | no | `localhost` | bind address (`0.0.0.0` binds all interfaces) |
| `max_events` | no | `64` | epoll event batch size |
| `max_clients` | no | `1024` | max simultaneous client fds tracked |

### Certificates

`auth/` ships only a CA certificate (`cacsertificate.crt`), not its private
key, so it can't be used to mint new server certs. For local testing, generate
your own throwaway CA and a server key/cert signed by it (`auth/generate_keys.sh`
generates an RSA-1024 key + CSR; you still need to self-sign or run it through
your own CA — `openssl x509 -req ... -CA <your-ca>.crt -CAkey <your-ca>.key`).
Point `ca_path` (client-side) at whatever CA cert you used to sign the server
cert.

### Run it

```sh
PROJECT_DIR=$(pwd) \
cert_path=auth/server_cert.pem \
key_path=auth/server_private_key.pem \
listen_port=4321 \
address=0.0.0.0 \
./build/tetrisd
```

`tetrisd` is single-process and epoll-driven (`src/tetrisd/main.c`): one
listening socket, non-blocking client fds, `SIGINT`/`SIGTERM` trigger a clean
shutdown of all open connections.

## Running the client (`tetrisu`)

### Configuration

Same env-var mechanism (`src/tetrisu/config_var.c`):

| Variable | Required | Default | Meaning |
|---|---|---|---|
| `PROJECT_DIR` | yes | — | base directory for relative paths below |
| `ca_path` | yes | — | path to the CA certificate used to verify the server |
| `listen_port` | no | `4321` | server port to connect to |
| `address` | no | `localhost` | server address to connect to |

### Run it

```sh
PROJECT_DIR=$(pwd) \
ca_path=auth/ca.crt \
listen_port=4321 \
address=localhost \
./build/tetrisu
```

`tetrisu` connects, performs the handshake, then drops into a `>` prompt:
each line typed is wrapped in an `htttp` request, encrypted, and sent; the
server's response is decrypted, parsed, and printed as
`Server response: <status> <reason> <body>`. EOF (Ctrl-D) exits.

## Wire protocol

Every message on the wire — handshake and application data alike — is framed
as:

```
[4-byte big-endian length][payload, ≤ FRAME_MAX bytes]
```

`FRAME_MAX` is `64 KiB - 4 bytes` (`include/tetrissh.h`).

### 1. Handshake (per connection, before any application data)

1. **Client → Server**: a 32-byte random nonce (generated the same way as a
   session key), length-prefixed.
2. **Server → Client**: the nonce signed with the server's RSA private key
   using RSA-PSS/SHA-256 (`tetrish_server_sign_nonce`), then the server's
   X.509 certificate — each length-prefixed.
3. **Client**: verifies the certificate against its trusted CA
   (`verify_server_cert` — full chain-of-trust + validity-period check via
   OpenSSL's `X509_STORE`), then verifies the signature over the nonce it
   sent using the certificate's public key.
4. **Client**: generates a fresh 32-byte session key (16-byte AES-128 key +
   16-byte HMAC key), encrypts it with the server's public key using
   RSA-OAEP/SHA-256, and sends it, length-prefixed.
5. **Server**: decrypts the session key with its private RSA key
   (`tetrish_server_decrypt_session_key`).

From this point both sides hold the same 32-byte session key and every frame
is encrypted.

### 2. Session encryption ("Fernet-equivalent")

`session_encrypt`/`session_decrypt` (`src/common/common.c`) implement
AES-128-CBC with a random IV, authenticated with HMAC-SHA256:

```
ciphertext = IV(16) || AES-128-CBC(plaintext) || HMAC-SHA256(32)
```

The HMAC is verified before decryption is attempted; a bad tag decrypts to
`NULL`. This whole blob is what gets length-prefixed and put on the wire via
`tetrish_send_frame`/`tetrish_recv_frame`.

### 3. Application payload (`libhtttp`)

Once authenticated, the plaintext carried inside each encrypted frame is an
`htttp_message_t` (`include/htttp.h`) — request (`method`, `path`, headers,
body) or response (`status`, `reason`, headers, body) — serialized to bytes
by `htttp_serialize` / parsed back by `htttp_parse`. Limits: 64 KiB message,
32 headers, 256-byte paths, 256-byte header values.

## The shell (`tetrish`) and system programs

`tetrish` (`src/tetrish/{main,shell}.c`) is a standalone line-oriented shell:
on startup it sources `.tetrishrc` from the current directory (if present),
then reads from stdin. Builtins are resolved first; anything else is
`fork`/`execvp`'d. `.tetrishrc` supports `PATH=...` and `setenv KEY=value`
(see `sample.tetrishrc`). This config path is independent of the `tetrisd`/
`tetrisu` environment-variable config described above.

The system programs under `src/tetrish/system_programs/` are meant to be run
from within `tetrish`:

- `ld`, `ldr` — permission-aware directory listing (`ldr` recurses); both
  link `perms` and call `perms_to_string` for the permission-string
  formatting. `find` links `perms` too but doesn't actually call it — its
  recursive filename search is a plain `strstr` match on `d_name`.
- `backup` — forks/execs `tar` to archive `$BACKUP_DIR` into
  `$PROJECT_DIR/archive/backup-<timestamp>.tar.gz`.
- `sys` — prints OS/kernel/hostname, memory, CPU, and uptime info from
  `uname`/`/proc`.
- `hash` — stub (`main` returns `0`); installs with the output name `#`.
- `dcheck`, `dspawn` — a daemon-testing pair: `dspawn` double-forks and
  writes a heartbeat log to `$PROJECT_DIR/dspawn.log`; `dcheck` greps
  `ps -efj` for the resulting `dspawn` process.

## Implementation status

- **`tetrisd`/`tetrisu`**: handshake, session encryption, and framing are
  fully implemented and functional end-to-end. Request *handling* on the
  server is currently a stub for demonstration: any authenticated message is
  decrypted, printed to the server's stdout, and answered with a fixed
  `200 OK "Accepted"` response (`src/tetrisd/state.c`) — there is no routing
  by `method`/`path` yet. The client similarly sends a placeholder
  `method`/`path` (`SOMEMETHOD /insert/path/here`) rather than a real command
  (`src/tetrisu/main.c`).
- **`tetrisctl`**, **`tetrislogd`**: stubs (`main()` returns `0`).
- **`libtetrisbrain`**: stub (`dummy()` only).
- **`common.h`**'s file header still describes it as being for a "Secure FTP
  project" — stale, but the code is genuinely load-bearing for `tetrish`'s
  transport layer.
- **Gap vs. the course spec**: the assignment describes `tetrisd` as a
  concurrent *game* server (multiplayer rooms, `SIGHUP`/`SIGUSR1` handling
  alongside `SIGINT`/`SIGTERM`, an admin control plane for `tetrisctl`,
  log-forwarding to `tetrislogd` over IPC) and `libhtttp` as carrying a fixed
  game-command set (`JOIN`/`LEAVE`/`START`/`MOVE`/`ROTATE`/`DROP`/`STATE`).
  None of that exists yet: `tetrisd` only installs handlers for `SIGINT` and
  `SIGTERM`, has no admin/IPC socket of any kind, and
  `htttp_message_t.method` (`include/htttp.h`) is a free-form string with no
  command enum or whitelist — the parser accepts anything before the first
  space. `libtetrisbrain` (piece/board/scoring logic) is untouched.

See `.claude/rules/clang-design.md` for coding conventions and open design
questions, and `.claude/rules/testing.md` for the (currently nonexistent)
testing story.
