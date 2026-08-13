# tetrisd control channel

`tetrisd` exposes an admin control plane on a Unix-domain socket
(`SOCK_STREAM`), separate from the public TCP listener. `tetrisctl` is its
client. The socket path is the `tetrisd_control_ipc` directive in
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

**Session.** A control connection is read like a player's: any number of
request frames, one framed HTTTP response each, until the peer closes. The
daemon never closes it first — `tetrisctl` sends one request, prints the
response and exits, and the daemon notices the hangup on the next tick and
reaps the connection then. EOF fails the connection the way it does for a
player, so a peer that closes mid-request loses whatever it had queued.

Exactly one control connection is held at a time; while it is live the daemon
accepts and immediately closes any other, so a busy client sees EOF. Both
queues have capacity 1, so the connection carries one request in flight per
tick — a client that pipelines is served one request per tick, not refused.

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
{"pid":1234,"players_connected":17,"players_authed":12,
 "players_capacity":1008,"fds_used":25,"fds_capacity":1024,"listen_port":4321}
```

`players_connected` and `players_authed` are bounded by `players_capacity`
(`tetrisd_max_player_fd`). `fds_used` counts every epoll-registered fd,
including both listeners and any control connection, and is bounded by
`fds_capacity`, the effective (possibly rlimit-clamped) `tetrisd_max_fds`.

Reload re-applies only directives that need no reallocation of live state, and
`config_var.h` is the authoritative list. `cert_path` and `key_path` are
replaced validate-then-swap, so a connection mid-handshake fails and reconnects
while established sessions are unaffected. `log_ipc` and
`tetrisd_logger_reconnect_seconds` take effect at the logger's next reconnect.
`tetrisd_client_capacity` and `tetrisd_max_player_fd` apply to connections
accepted after the reload — live ones keep the sizes they were accepted with,
see `layers.md` — and `tetrisd_room_tick_hz` applies at the next room tick.

Everything that sizes a table or binds a socket requires a restart:
`tetrisd_max_fds`, `tetrisd_max_events`, `tetrisd_max_rooms`,
`tetrisd_logger_capacity`, `listen_port`, `tetrisd_address`,
`tetrisd_control_ipc`. A reload that fails validation logs a warning and leaves
the running config untouched, including a `tetrisd_max_player_fd` that exceeds
`tetrisd_max_fds`; one that passes is still clamped to the running
`tetrisd_max_fds`, since the rlimit clamp runs only at startup.

## tetrisctl

```
tetrisctl [--socket PATH] [--timeout MS] [--raw] <status|shutdown|reload>
```

Reads `tetrisd_control_ipc` from `$PROJECT_DIR/.tetrishrc` unless `--socket` overrides
it, sends one framed request, prints the response body, and exits. The body is
pretty-printed JSON by default; `--raw` writes it byte-exact with no added
newline, so it can be piped to a hash or a parser and match what the daemon
sent. `--help` prints the usage on stdout and exits `0`.

| Exit | Meaning |
|---|---|
| `0` | the daemon answered 2xx |
| `2` | `PROJECT_DIR` or `.tetrishrc` unusable |
| `3` | cannot reach the daemon |
| `4` | request failed, or the reply was unreadable |
| `5` | the daemon answered, with a non-2xx |
| `64` | usage error |

The split between `3`, `4` and `5` is what lets a script tell "the daemon is
not there" from "the daemon refused the command".

Every stage is deadlined by `--timeout` (default 3000 ms), `connect(2)`
included: it is issued non-blocking and bounded with `poll`, because
`SO_SNDTIMEO` does not cover it and a daemon stopped before its accept loop
would otherwise hang the tool indefinitely. `SIGPIPE` is ignored, so a send
landing on a connection the busy policy has already closed is reported rather
than killing the process.

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
3. **fd-budget reservation.** Player admission is capped at `cfg.max_player_fd`
   (`acceptor.c`) while the epoll table is sized `cfg.max_fds`, clamped to
   `RLIMIT_NOFILE` in `server_init`. The gap between the two is the reservation:
   it is what a control `accept(2)` draws on once the player table is full. The
   gap is the operator's to configure — set `tetrisd_max_player_fd` equal to
   `tetrisd_max_fds`
   and control accepts start failing as soon as players fill the table, which
   `Control_accept` reports and survives rather than treating as fatal.
4. **No crypto.** Control requests skip the tetrissh handshake, so a flood of
   half-handshaken TCP clients adds no latency to control requests.

Measured (dev container), against the earlier two-connection implementation:
with `RLIMIT_NOFILE=128`, `max_fds` clamped to 112 and ~300 held TCP
connections saturating the player table, `tetrisctl status` and `tetrisctl
shutdown` both answered in ~2–5 ms. Not re-measured since the single-connection
rework.

## Planned: command dispatch moves to the server

Not implemented. Recorded here because the current arrangement does not extend,
and the reason is worth having written down before someone adds a command.

Today `Control_process` parses the request, routes it, builds the response and
stages it — so routing knowledge lives in `control.c`. That works only while
every command is answerable from the control layer plus a snapshot the server
hands in. It stops working as soon as commands act on the rest of the daemon:
closing a room, force-disconnecting a player, draining the acceptor. Those need
`AppData`, `PlayerIo`, and the close sets, none of which control can reach, and
none of which it should.

The symptom is already visible. `server_tick` renders the state dump whenever
*any* control request is queued, because `Control_has_request` reports that a
frame exists, not what it asks for — so `POST /reload` and `POST /shutdown` pay
for a render, and its player scan, that they never read.

The intended split is transport versus meaning. Control keeps the socket,
accept, framing, HTTTP parse and serialize, response staging, close and reset,
and loses all command knowledge. A `server_process_control` owns a route table
whose handlers may touch any layer:

```c
/* control.h — control iterates requests, the server writes answers back */
bool Control_next_request(ControlData* data, size_t idx, HtttpRequest* out);
int  Control_respond(ControlData* data, HtttpStatus status, const char* body, size_t body_len);
bool Control_idle(const ControlData* data);   /* queue drained, writer idle */

/* server.c */
typedef HtttpStatus (*ControlHandler)(Server*, const HtttpRequest*,
                                      char* body, size_t body_size, size_t* body_len);
static const ControlRoute ROUTES[] = {
    { "GET",  "/status",   handle_status },
    { "POST", "/reload",   handle_reload },
    { "POST", "/shutdown", handle_shutdown },
};
```

Handlers receive the parsed request, so a later `POST /room/close` reads its
target from the body and calls into the owning layer directly. `404` and `405`
become route-table misses in the server. `handle_status` calls
`render_state_json` itself, so only the command that needs the dump pays for it.

Respond-then-act moves out with the rest. `shutdown_requested` and
`ControlActions` are command knowledge; under this split `handle_shutdown` sets
`server->pending_shutdown` and the tick ends with

```c
if (server->pending_shutdown && Control_idle(&server->control)) return -1;
```

so control never learns what a shutdown is, and `handle_reload` sets
`should_reload_config` directly.

The boundary holds only if `Control_respond` is the sole sanctioned way to touch
`write_q`. Once a handler reaches into `conn` the separation is gone, and the
one-response-per-request accounting goes with it.

## Planned: the close set becomes a single fd

Not implemented. Left over from the two-connection design.

The pairing at the end of the tick is not the leftover and should not be
collapsed:

```c
Control_close(&server->control, &server->epoll.control_close_fds);
Epoll_close(&server->epoll, &server->epoll.control_close_fds);
```

That is the fd ownership split, the same one players use. `Control_close` frees
the layer's own state; `Epoll_close` owns the `close(2)` and drops the table
entry. Merging them would put fd ownership back inside the layer, which is
exactly what centralising it in the epoll layer was for.

What is left over is `control_close_fds` being a `SparseSet_bool` sized
`max_fds` (`epoll.c`), when at most one fd can ever be in it. That bound is
provable, not incidental: `Control_accept` refuses to admit while
`conn.fd != -1`, and it runs before `Control_hangup` in the tick, so a live
connection blocks any second fd from appearing. Every other writer —
`Control_read`, `Control_process`, `Control_write`, `Control_hangup` — marks the
same `conn.fd`, and the registration-failure path marks a freshly accepted fd,
which only exists when there was no connection to begin with.

The cost is memory rather than time. `Epoll_close` and `Epoll_reset` both
iterate the dense array, which is empty on almost every tick, so there is no
per-tick penalty; it is the two allocations of `max_fds` entries each, tens of
KB at the default, standing in for one `Fd`.

The fix completes a family epoll has already grown for singleton entries.
`Epoll_accept_one`, `Epoll_set_interest` and `Epoll_erase_one` exist; the
missing member is an `Epoll_close_one(data, fd)` that closes and erases.
`control_close_fds` then becomes a plain `Fd`, `-1` when nothing is closing, and
`Epoll_close` over a set belongs to players alone — the only layer that
genuinely has many. See `logger.md` for the same single-fd argument applied to
the logger, which needs erase-without-close rather than this.

The knock-on is that the four `SparseSet_bool_contains` guards inside
`control.c` become a comparison against one fd, or a flag on the connection.
