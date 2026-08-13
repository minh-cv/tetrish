# tetrisd logging

`tetrisd` logs through `LoggerData` (`src/tetrisd/logger_layer.{c,h}`), the outbound half of the log pipeline: a nonblocking Unix socket to `tetrislogd`, fed by a bounded queue of owned log records. `LoggerData_init` installs the process log handler, so every `LOGGER_LOG` after that point appends to the queue rather than going anywhere directly.

It is a singleton rather than a per-fd layer, so it carries its own fds instead of sparse sets keyed by fd. It has no `_reset`: the queue is a backlog that must survive both ticks and reconnects, like PlayerIo's `write_qs`.

Implemented and working. What follows is what should change, and why; none of it is done.

## The layer does not follow the lifecycle

Every other layer emits descriptions — close sets, write-queue status, `ControlInterest` — and lets `server_tick` apply them to epoll. The logger does not. Three file-static functions in `server.c` sequence its stages themselves and call epoll directly:

| static | what it does | why it is wrong |
|---|---|---|
| `logger_connect` | connect or arm the retry timer, then `Epoll_accept_one` | acquisition and epoll registration in one step, in the wrong file |
| `logger_disconnect` | `Epoll_erase_one` then `LoggerData_close` | ordering that belongs to the close fan-out |
| `logger_tick` | accept, read, write, sync and close as one blob at the end of the tick | replaces the tick's stage order with its own |

All three should be deleted. The state transitions belong in `logger_layer.c` as `LoggerData_*` functions — most already exist — and the epoll calls belong at `server_tick`'s existing stage call sites, where the dataflow is visible the way `layers.md` asks for.

One file-static does stay: `g_logger` in `logger_layer.c`. `logger_set_log_handler` takes no context parameter, so the handler reaches the instance through that pointer, and `LoggerData_init` asserts there is only one per process. That is forced by the corestack API, not a layering choice, and it is unrelated to the three above.

## The conforming shape

* **accept** — socket down and the retry timer not armed: connect, hand the fd back for registration.
* **read** — the retry timerfd is readable: drain it and disarm, so the next tick's accept reconnects.
* **write** — drain the record queue, alongside `PlayerIo_write` and `Control_write`.
* **sync** — interest from `LoggerData_wants_write`, where `Epoll_set_interest` already runs for control.
* **close** — hangup, or a write that failed, retires the fd and arms the backoff.
* **reset** — none, deliberately, per above.

Write sits at the normal write stage rather than last. Records produced by later stages — notably the control-shutdown line — then wait a tick, which is acceptable because nothing is lost: `LoggerData_free` flushes on the way out with a bounded budget (ten attempts paced by `poll`) and writes the remainder to stderr. Deferring the write to the end of the tick would only fix records produced by close and reset, while leaving the general case untouched: a disconnected logger already waits on its retry timer, and a backed-up socket on `EPOLLOUT`.

That flush is therefore exercised on every clean shutdown rather than being a rare fallback, which is worth keeping true — a path that only runs under failure tends to rot.

## What it reports

The layer's epoll needs reduce to one erase and one watch per tick. Both halves of that need an argument, and they are separate arguments.

**At most one watch.** Registration happens in exactly one place, the accept stage, and that stage runs once per tick. Today it can happen twice — `logger_tick` connects, the first write on the fresh socket fails, and it erases and reconnects inside the same tick — which is only possible because the write stage is allowed to reconnect. Once a failed write merely closes and leaves reconnection to the next tick's accept, the second registration disappears.

**At most one erase.** Only the close stage retires an fd, and there is never more than one socket fd live: the queue is drained through a single connection, and `LoggerData_accept` is only called when `fd == -1`. The retry timerfd is not part of this count at all, because it is created once at init and stays registered for the life of the process — armed or disarmed, never added or erased.

Both can happen in the same tick, and that is not a contradiction. Accept runs before close, so a tick can connect and then have that same fd fail its first write and be retired by close. The net effect on the table is nothing, and doing it in that order is still correct — `Epoll_erase_one` deliberately issues no `EPOLL_CTL_DEL`, since closing the fd already removes it from the interest list.

Because each is bounded at one and they happen at different stages, there is no delta struct and nothing is deferred. Each stage issues its epoll call where it stands, the way control already does:

```c
/* accept fan-out */
const Fd logger_fd = LoggerData_accept(&server->logger, &server->cfg);
if (logger_fd != -1 &&
    ((size_t)logger_fd >= server->epoll.entries.capacity ||
     Epoll_accept_one(&server->epoll, logger_fd, EPOLL_ENTRY_LOGGER, 0) == -1)) {
    LoggerData_watch_failed(&server->logger, &server->cfg);
}

/* close fan-out */
const Fd retired = LoggerData_close(&server->logger, &signals);
if (retired != -1) {
    Epoll_erase_one(&server->epoll, retired);
}
```

The erase is the one place the logger differs from control. A control fd goes into `control_close_fds` and `Epoll_close` both closes it and drops the entry; the logger closes its own fd, which is what `Epoll_erase_one` exists for, so the close stage needs an erase-without-close on the fd the layer just retired rather than a close set.

### Why registration failure needs a call back into the layer

Watching the fd can fail two ways: the fd number is past the epoll table's range, or `epoll_ctl` itself fails on resource exhaustion. Both are rare. Both must still be handled, because the layer's state machine reads the answer — it treats an unwatchable socket as a failed connection attempt and falls back to the retry timer.

The out-param cannot carry that answer, and the reason is ordering. The layer produces `LoggerEpollDelta` and returns; only then does the server attempt the registration. By the time the result exists, the layer has already committed to its next state.

Leaving it unreported is not an option, because the fd stays open while nothing polls it. Writes would still go out — the write stage is readiness-independent — but `EPOLLHUP` would never arrive, so a dead `tetrislogd` would go unnoticed, and `EPOLLOUT` would never arrive, so a socket that filled once would never be drained again. The layer would sit believing it is connected, which is precisely the invariant `fd != -1` is supposed to mean.

Three ways out, and why the middle one:

* The layer holds an `EpollData*` and registers for itself. This is what `logger_connect` does today, and it is the coupling the whole change exists to remove — no other layer knows epoll exists.
* The server reports the failure back with `LoggerData_watch_failed(data, cfg)`, which closes the fd and arms the backoff. One extra function and one branch at the call site, and the layer keeps ownership of its own transitions.
* The server closes the fd itself and leaves the layer to notice. This puts a logger state transition in `server.c`, which is the thing being moved out, and it reintroduces the question of who arms the timer.

## Fatal at init, recoverable at runtime

The retry timerfd should be created once in `LoggerData_init`, and failing there should be fatal, like `Acceptor_init` already is. `timerfd_create` fails only on `EMFILE`/`ENFILE`/`ENOMEM`, which at startup is a genuine cannot-run condition; afterwards, arming and disarming is `timerfd_settime` on an existing fd, which cannot meaningfully fail. The timerfd then becomes a permanent epoll entry like the two listeners, never registered or erased, and the accept stage only ever hands back a socket fd.

This fixes a live defect. Today a failed `timerfd_create` leaves both `fd` and `timerfd` at `-1`, and `logger_tick` comments that "the next tick tries again instead" — but `Epoll_poll` calls `epoll_wait` with a `-1` timeout, so on an idle server there is no next tick. Logging stops permanently and silently, which is the worst failure mode for the one subsystem whose job is reporting what happened.

The general rule, which the room timer should follow too: acquire at init and fail there if you cannot; at runtime, fail only when the alternative is silent incorrectness. The acceptor already splits this way — `Acceptor_init` failing aborts startup, while `EMFILE` at runtime sets `should_stop_accepting` and recovers when a slot frees. A room timer that cannot tick is silent total failure of the daemon's purpose, so it belongs on the fatal side. Losing the log transport does not, which is why the logger keeps degrading rather than exiting — but degrading has to mean retrying, not stalling.
