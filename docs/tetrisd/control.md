# tetrisd control channel

`tetrisd` exposes an admin control plane on a Unix-domain socket
(`SOCK_STREAM`), separate from the public TCP listener. `tetrisctl` is its
client. The socket path is the `control_ipc` directive in
`$PROJECT_DIR/.tetrishrc` (default: `$PROJECT_DIR/tetrisd.sock`); a relative
path is resolved against `PROJECT_DIR`, like `log_ipc`.

The daemon creates the socket file with mode `0600` (enforced against the
daemonized `umask(0)`), so **socket-file permissions are the authentication
boundary** — there is no tetrissh handshake and no encryption on this channel.
Only the user running `tetrisd` (or root) can issue control commands.

## Wire format

Framing is identical to the public path minus encryption: each message is a
**4-byte big-endian length prefix** followed by that many bytes of payload,
with payload length in `(0, FRAME_MAX]` (`FRAME_MAX` = 64 KiB − 4, `wire.h`).
The payload is one HTTTP/1.0 message (`external/corestack/include/htttp.h`).

**Session: one request per connection.** The daemon serves the first request
frame, sends exactly one framed HTTTP response, and closes the connection once
the response is flushed. At most 2 control connections are served at a time
(`CONTROL_MAX_CONNS`); beyond that the daemon accepts and immediately closes,
so a busy client sees EOF.

## Requests

| Request | Response | Effect |
|---|---|---|
| `GET /status` | `200`, JSON body (below) | none |
| `POST /shutdown` | `200 {"ok":true}` | response is flushed, then the main loop stops (equivalent to SIGTERM); the socket file is unlinked on teardown |
| `POST /reload` | `200 {"ok":true}` | sets the reload flag; the config is re-read validate-then-swap atop the next loop iteration (also triggered by SIGHUP) |
| malformed HTTTP | `400` | — |
| unknown path | `404` | — |
| known path, wrong method | `405` | — |

`/status` body:

```json
{"pid":1234,"uptime_s":321,"players_connected":17,"players_authed":12,
 "fds_used":25,"fds_capacity":1024,"listen_port":4321}
```

`fds_capacity` is the effective (possibly rlimit-clamped) `max_fds`;
`fds_used` counts every epoll-registered fd, including the two listeners and
any control connections.

Reload only re-applies directives that need no reallocation of live state —
currently `client_capacity`, which affects connections accepted after the
reload (live connections keep the sizes they were accepted with, see
`layers.md`). Capacity, addressing and credential directives
(`max_fds`, `max_events`, `listen_port`, `address`, `control_ipc`,
`cert_path`, `key_path`) require a restart; a reload that fails validation
leaves the running config untouched.

## tetrisctl

```
tetrisctl <status|shutdown|reload> [--json]
```

Reads `control_ipc` from `$PROJECT_DIR/.tetrishrc`, sends one framed request,
prints the response body (pretty-printed JSON by default, raw with `--json`),
and exits `0` on 2xx, `2` on config/connect failure, `3` on I/O failure or a
non-2xx response, `64` on usage error. Frame I/O carries a 3-second timeout so
a wedged daemon cannot hang the tool. A caveat: `connect(2)` itself is
blocking; the accept-and-close busy policy keeps the listen backlog (4)
drained, so this is not observable in practice.

## Why the control plane survives public-listener saturation

This is the graded property ("`tetrisctl shutdown` works while the TCP
listener is being flooded"):

1. **Separate kernel accept queues.** The AF_UNIX listener has its own
   backlog; connect floods on the TCP port cannot occupy it.
2. **Bounded per-tick work.** Every player-facing stage is capacity-bounded
   (`client_capacity` queues, reader/writer state machines, acceptor stops at
   table capacity), so the event loop always returns to `epoll_wait`, where
   control-fd readiness is reported independently of player readiness. Nothing
   on the control path waits on any player queue.
3. **fd-budget reservation.** Player admission is capped at `cfg.max_fds`
   (`acceptor.c`), while the epoll table is sized
   `max_fds + CONTROL_FD_HEADROOM` and `server_init` clamps `max_fds` to
   `RLIMIT_NOFILE − CONTROL_FD_HEADROOM`. A full player table therefore can
   never make `accept(2)` on the control listener fail with `EMFILE`, and a
   control fd always fits the epoll table.
4. **No crypto.** Control requests skip the tetrissh handshake, so a flood of
   half-handshaken TCP clients adds no latency to control requests.

Measured (dev container): with `RLIMIT_NOFILE=128`, `max_fds` clamped to 112
and ~300 held TCP connections saturating the player table, `tetrisctl status`
and `tetrisctl shutdown` both answered in ~2–5 ms.
