# The control plane: `tetrisctl` and its integration into `tetrisd`

Design document. Nothing here is implemented yet; `src/tetrisctl/main.c` is still `return 0`.

## Requirements this design answers

From the spec (`specs/tetrish-requirements.html`, sections on `tetrisctl` and the control plane):

* `tetrisctl` is a separate binary running administrative actions against a live `tetrisd`.
* At minimum: a status query and a graceful shutdown trigger.
* The channel is a real local-only IPC mechanism, never the public TCP port and never a function call.
* The wire format on that channel is HTTTP, and that part is fixed.
* The control plane stays available while the public TCP listener is saturated. `tetrisctl shutdown` must work while the server is being flooded.
* Every admin action is logged with a timestamp.

## Transport

Unix domain stream socket (`AF_UNIX`, `SOCK_STREAM`) bound at the path in the `ctl_ipc` directive of `.tetrishrc`, resolved through `config_get_path` like `log_ipc`.

Framing is the existing one: 4-byte big-endian length prefix (`wire.h`) followed by a serialized HTTTP message. The payload is plaintext — there is no `tetrissh` handshake on this channel. That means `Reader`/`Writer` from corestack drive the daemon side unchanged, and `tetrish_send_frame`/`tetrish_recv_frame` with `key = NULL` drive the CLI side, exactly as `tetrislogd` already does.

Authorization is filesystem-based, which is what makes dropping the handshake sound:

* The daemon `bind`s, then `chmod`s the path to `0600`, then `listen`s. Binding under a restrictive `umask` is not enough on every platform, so the explicit `chmod` is the contract.
* On startup, a pre-existing socket file is probed with a non-blocking `connect`. `ECONNREFUSED` means stale, so unlink and rebind; a successful connect means another `tetrisd` owns it, which is a fatal init error. Never unlink blindly.
* `server_free` unlinks the path.
* Each accepted control connection is checked with `SO_PEERCRED`; a peer uid that is neither the daemon's euid nor 0 gets a `403` and is closed. This is defence in depth behind the `0600` mode.

## Daemon-side shape

The control plane is a second pipeline running beside the player pipeline, built from the same layer types, with no shared per-fd state. Sharing types but not instances is what keeps player congestion out of the control path.

### Prerequisite renames

`PlayerIo` contains nothing player-specific — it is a `Reader`/`Writer` pair plus read/write queues keyed by fd. Rather than copy it, rename it to `FrameIo` (`src/tetrisd/frame_io.{h,c}`) and instantiate it twice. Same reasoning for `EpollData`, whose `player_close_fds` member and `Epoll_poll`'s `player_read`/`player_write` parameters become `close_fds`/`read_fds`/`write_fds`. `HtttpData` is already neutral. These are mechanical renames with no behavioural change, and they are a precondition for everything below.

### New members of `Server`

```c
typedef enum {
    SERVER_RUNNING,
    SERVER_DRAINING,   // no new players admitted, existing ones keep playing
    SERVER_STOPPING,   // shutdown acknowledged, flushing the ctl response
} ServerLifecycle;

typedef struct {
    struct config_var cfg;

    Acceptor acceptor;
    EpollData epoll;
    FrameIo player_io;
    AuthData auth;
    HtttpData htttp;
    AppData app;

    CtlAcceptor ctl_acceptor;
    EpollData ctl_epoll;
    FrameIo ctl_io;
    HtttpData ctl_htttp;
    CtlData ctl;

    ServerLifecycle lifecycle;
    struct timespec stop_deadline;
} Server;
```

Every control-side sparse set is still allocated with capacity `cfg.max_fds`, because sparse sets are indexed by fd number and a control fd can land anywhere in the process fd space. The admission cap (`cfg.max_ctl_fds`) is a separate, much smaller number.

### New files

* `src/tetrisd/ctl_acceptor.{h,c}` — `CtlAcceptor`: the `AF_UNIX` listener, socket-file lifecycle, `SO_PEERCRED` check, admission cap. Same shape as `Acceptor`.
* `src/tetrisd/ctl_layer.{h,c}` — `CtlData`: the lift stage, command routing, and the effect outputs.
* `src/tetrisd/ctl_status.{h,c}` — `ServerStatus` and its snapshot builder.

### The lift stage

`HtttpData_parse` consumes `SparseSet_AuthFrameQueue`, whose element carries a `FrameStatus`. The control path has no auth layer, so a stage translates `ReaderFrameQueue` into `AuthFrameQueue`, mapping `READER_FRAME_OK` to `FRAME_OK` and `READER_FRAME_PAYLOAD_TOO_LARGE` to `FRAME_PAYLOAD_TOO_LARGE`. It is where transport-level status is normalized, which is the same job auth does on the player path, minus the crypto.

Ownership differs from the player path in one way that must not be got wrong. `AuthData_reset` frees the frames in `decrypt_qs` because decryption allocated them. The lifted frames are non-owning views into `ctl_io`'s `read_qs`, which `FrameIo_reset` frees. So `CtlData_reset` deactivates its lifted queues and frees nothing. Since nothing is freed there, the two resets have no ordering constraint between them.

```c
void CtlData_lift(
    CtlData* data,
    const SparseSet_ReaderFrameQueue* m_read_qs,
    SparseSet_AuthFrameQueue* m_lifted_qs,
    SparseSet_bool* err_fds
);
```

### Command routing

```c
typedef struct {
    SparseSet_CtlEntry entries;
    SparseSet_AuthFrameQueue lifted_qs;
    SparseSet_HtttpOutboundMessageQueue response_qs;

    bool shutdown_requested;
    bool drain_requested;
    Vec_Fd kick_fds;
} CtlData;

void CtlData_respond(
    CtlData* data,
    const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
    const ServerStatus* status,
    SparseSet_HtttpOutboundMessageQueue* m_response_qs,
    SparseSet_bool* err_fds
);
```

`CtlData_respond` takes a read-only `ServerStatus` snapshot rather than a back-pointer to `Server`, so the layer graph stays acyclic and the responder is testable on its own. Side effects leave as output data — `shutdown_requested`, `drain_requested`, `kick_fds` — which `server_tick` applies after the pass, in keeping with the commit-or-discard rule the other layers follow.

`ServerStatus` is a flat struct of scalars built each tick, only when a control connection is actually active, by reading `SparseSet_*_size` off the layers:

```c
typedef struct {
    pid_t pid;
    ServerLifecycle lifecycle;
    const char* address;
    int port;
    time_t uptime_seconds;
    size_t players_connected;      // player_io.entries
    size_t players_authenticated;  // auth entries in AUTH_DONE
    size_t ctl_connections;        // ctl_io.entries
    size_t rooms;                  // 0 until rooms exist
    size_t max_fds;
    size_t max_ctl_fds;
    bool accepting;
    size_t logs_dropped;
} ServerStatus;
```

The remaining lifecycle functions (`CtlData_init`, `_free`, `_reset`, `_accept`, `_close`) follow the layer API in `layers.md` verbatim.

## Keeping the control plane alive under flood

Three independent mechanisms; the requirement is not met by any one of them alone.

**Fd reservation.** `Acceptor_accept` currently receives `cfg.max_fds` as its admission limit. It receives `cfg.max_fds - cfg.max_ctl_fds` instead, so player connections can never consume the last few table slots. This also needs process-level headroom: `server_init` reads `RLIMIT_NOFILE` and fails fast if `max_fds` plus a margin exceeds it, because `Acceptor_accept`'s `EMFILE` path stops player admission but does nothing for a control `accept` that has no fd to return either.

**A nested epoll.** The player `epoll_wait` blocks with timeout `-1` and returns at most `max_events` fds; under flood the event array is full of player fds, and a control fd sitting in the same instance is only guaranteed to be reported within `ceil(ready / max_events)` waits. So the control plane gets its own `EpollData`, and `ctl_epoll.epoll_fd` is registered in the player epoll as `EPOLL_ENTRY_CONTROL` with `EPOLLIN` — an epoll fd is itself pollable, so any control readiness wakes the blocking wait, and the whole control plane occupies exactly one slot in the player event array. On top of that, `server_tick` drains `ctl_epoll` with timeout `0` unconditionally at the top of every tick, whether or not the player poll mentioned it. That makes control starvation impossible rather than merely bounded.

**Separate error sets.** `ctl_epoll.close_fds` is distinct from `epoll.close_fds`, so no player-side failure can mark a control fd, and the control chain runs to completion before the player chain starts.

Control-plane backpressure is its own admission cap: past `cfg.max_ctl_fds` live control connections, `CtlAcceptor_accept` accepts and immediately closes, so a `tetrisctl` invocation sees EOF instead of hanging in a backlog.

## Tick order

```
CtlEpoll drain (timeout 0)          -> ctl reading/writing lists, ctl acceptor readable
Epoll_poll (blocking)               -> player lists, acceptor readable, ctl epoll readable
  if ctl epoll readable: drain it again (timeout 0)

-- control chain --
CtlAcceptor_accept                  (cap max_ctl_fds; peercred check)
Epoll_accept / FrameIo_accept / HtttpData_accept / CtlData_accept   on ctl_*
FrameIo_read        (ctl_io)
CtlData_lift        read_qs      -> lifted_qs
HtttpData_parse     lifted_qs    -> ctl_htttp.parsed_qs
CtlData_respond     parsed_qs    -> response_qs, plus effects
HtttpData_serialize response_qs  -> ctl_io.write_qs
FrameIo_write       (ctl_io)
Epoll_sync_interest (ctl_epoll)
close fan-out on ctl_epoll.close_fds

-- apply effects --
drain_requested    -> lifecycle = SERVER_DRAINING
shutdown_requested -> lifecycle = SERVER_STOPPING, stop_deadline = now + shutdown_grace_ms
kick_fds           -> merged into epoll.close_fds

-- player chain --
unchanged, except: Acceptor_accept limit is max_fds - max_ctl_fds, and the
listener's EPOLLIN is disarmed whenever lifecycle != SERVER_RUNNING

-- resets, both chains --
```

The control chain runs first so a shutdown is observed in the same tick it arrives, before any player work is done.

## Shutdown sequencing

`POST /ctl/shutdown` does not exit inside the layer. `CtlData_respond` queues a `200` and sets `shutdown_requested`; `server_tick` moves the lifecycle to `SERVER_STOPPING` and stamps a deadline. The response is serialized into `ctl_io.write_qs` the same tick, and per the writing-is-readiness-driven rule in `layers.md` it goes out once `EPOLLOUT` is armed — normally the very next tick, since `Epoll_sync_interest` arms it and `epoll_wait` then returns immediately.

`main` becomes:

```c
while (running && !server_should_stop(&server)) {
    server_tick(&server);
}
```

where `server_should_stop` is true when the lifecycle is `SERVER_STOPPING` and either every control write queue is empty or the grace deadline has passed. The deadline exists because a `tetrisctl` that dies between sending and reading would otherwise wedge the daemon. `SIGINT`/`SIGTERM` keep the existing hard path through `running`.

`drain` is the weaker sibling: it stops admitting players and leaves everything else alone, and the process still exits only on a later `shutdown` or a signal.

## Control-channel protocol

One HTTTP request per frame, one response per request, connection closed by the client afterwards. Bodies are JSON (`Content-Type: application/json`), built with the already-vendored cJSON.

| Command | Request | Success |
| --- | --- | --- |
| `status` | `GET /ctl/status` | `200`, `ServerStatus` as a JSON object |
| `shutdown` | `POST /ctl/shutdown` | `200` `{"lifecycle":"stopping"}` |
| `drain` | `POST /ctl/drain` | `200` `{"lifecycle":"draining"}` |
| `players` | `GET /ctl/players` | `200`, JSON array of `{id, authenticated}` |
| `rooms` | `GET /ctl/rooms` | `200`, JSON array; empty until rooms exist |
| `kick` | `POST /ctl/kick`, body `{"id":N}` | `200`, or `404` if no such player |
| `reload` | `POST /ctl/reload` | `501` until config reload lands |

Errors: unknown path `404`, known path with the wrong method `405`, unparseable request or bad JSON `400`, oversized frame `413`, peer uid rejected `403`, responder failure `500`. The connection is not closed on a `4xx` — the client closes it. Every accepted command is logged before it takes effect, via `LOGGER_LOG(LOG_INFO, "ctl", ...)`, which is what satisfies the spec's admin-action logging requirement.

## The `tetrisctl` binary

```
src/tetrisctl/main.c            argv parsing, exit codes
src/tetrisctl/config_var.{h,c}  reads PROJECT_DIR/.tetrishrc for ctl_ipc only
src/tetrisctl/command.{h,c}     table of name -> {method, path, body builder, response printer}
src/tetrisctl/transport.{h,c}   connect, send one frame, receive one frame
```

Usage is `tetrisctl [--socket PATH] [--timeout MS] [--raw] <command> [args]`. It is a straight blocking one-shot: connect, `htttp_serialize` the request, `tetrish_send_frame(fd, buf, len, NULL)`, `tetrish_recv_frame(fd, &len, NULL)`, `htttp_parse`, print, exit. No epoll, no handshake, no daemonizing. `SO_RCVTIMEO`/`SO_SNDTIMEO` default to 3000 ms so a wedged daemon cannot hang the CLI.

Exit codes: `0` for `2xx`, `1` for a usage error, `2` for connect failure or timeout (which is also what "daemon not running" looks like — `ENOENT` or `ECONNREFUSED` on the socket path), `3` for `4xx`, `4` for `5xx`. This is what makes `tetrish` builtins like `tetris-status` composable.

Links against `htttp tetrissh config common cjson`.

## Config additions

In `src/tetrisd/config_var.c`, per the per-daemon config convention:

* `ctl_ipc` — path, resolved against `PROJECT_DIR`, default `var/socket/tetrisd_ctl`. Required; init fails without a usable path.
* `max_ctl_fds` — default `4`, minimum `1`. Both the concurrent control-connection cap and the reserved fd count.
* `ctl_capacity` — default `4`, minimum `1`. Per-control-connection queue capacity, the control analogue of `client_capacity`.
* `shutdown_grace_ms` — default `2000`, minimum `0`.

`src/tetrisctl/config_var.c` reads `ctl_ipc` alone, with `--socket` overriding it.

## CMake

`tetrisd` gains `ctl_acceptor.c`, `ctl_layer.c`, `ctl_status.c` and links `cjson`. `tetrisctl` becomes a real target:

```cmake
add_executable(tetrisctl src/tetrisctl/main.c src/tetrisctl/config_var.c
                         src/tetrisctl/command.c src/tetrisctl/transport.c)
target_include_directories(tetrisctl PRIVATE src/tetrisctl include)
target_link_libraries(tetrisctl PRIVATE cjson htttp common tetrissh config)
```

## Implementation order

1. The `PlayerIo` → `FrameIo` and `EpollData` renames, with the player pipeline still passing.
2. `CtlAcceptor` plus the socket-file lifecycle, the nested epoll registration, and the fd reservation — verified by connecting with `nc -U` and watching the accept and close.
3. `CtlData_lift` and a `CtlData_respond` that only serves `status`, wired end to end.
4. `tetrisctl status`.
5. `shutdown`, the lifecycle enum, `server_should_stop`, and the grace deadline.
6. `drain`, `players`, `kick`.
7. The flood test: saturate the TCP listener up to `max_fds - max_ctl_fds` and confirm `tetrisctl shutdown` still returns.
