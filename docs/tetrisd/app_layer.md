# Application layer of `tetrisd`

Design for replacing the echo in `AppData_respond` with player, room and game
logic. Conventions are the ones in `layers.md`; this document only records where
the application layer needs more than those conventions give, and why.

## Scope

V1 of `docs/plan.md`: player identity and names, room create/join/leave,
host-driven start, a server-side game loop stepping `libtetrisbrain`, per-frame
input, garbage between seats, and state broadcast. Public room listing, host
transfer, spectators and chat are V2/V3 and are only accounted for in the shape
of the types, not implemented.

## What the pipeline does not already give

The four stages before the application layer are per-fd and request-shaped: one
input frame on an fd produces at most one output frame on the same fd. The
application layer breaks all three parts of that.

* Fan-out. One request (`join`) produces a reply to the sender and an event to
  every other member of the room.
* Unsolicited output. The game loop emits frames on fds that had no input this
  tick, driven by a timer rather than by a socket read.
* Non-fd-keyed state. A room is keyed by room id and outlives any particular
  connection; the layer conventions only describe `{Fd: [Object]}`.

Nothing below changes the neighbouring layers. `HtttpData_serialize` already
iterates whatever fds are active in `m_response_qs`, so writing into a slot the
application layer did not read from this tick works as is, provided the fd is
accepted and not already in `err_fds`.

## Module split

The layer is one layer to the server, four translation units internally. The
split exists so the game rules are reachable without a socket.

```
src/tetrisd/app_layer.{h,c}   layer shell: init/accept/respond/tick/close/reset/free
src/tetrisd/app/command.{h,c} HtttpMessage -> AppCommand (cjson)
src/tetrisd/app/event.{h,c}   AppEvent -> HtttpMessage (cjson)
src/tetrisd/app/world.{h,c}   players, rooms, command application, effect emission
src/tetrisd/app/game.{h,c}    per-room frame stepping over libtetrisbrain
```

`world` and `game` see no fds, no HTTTP, no syscalls and no allocation after
init. They take a `PlayerRef` and an `AppCommand` and push `AppEffect`s into a
sink. `app_layer` is the only place that knows fds, and the only place that
allocates outbound messages.

`tetrisd` gains `tetrisbrain` and `cjson` in `target_link_libraries`.

## Identity

`PlayerRef` is a generational handle, not a raw fd:

```c
typedef struct { uint32_t index; uint32_t generation; } PlayerRef;
typedef struct { uint32_t index; uint32_t generation; } RoomRef;
```

`index` is the fd number for players and the room slot for rooms, so both
directions are O(1) with no allocation. `generation` is bumped on close, which
makes a stale reference held by a room detectable rather than a silent
misroute onto whoever inherited the fd number. Every dereference goes through
`world_player(World*, PlayerRef)` returning `NULL` on generation mismatch;
nothing indexes `players`/`rooms` directly.

The room code the user types (six characters) is a separate short string mapped
to `RoomRef` by a small open-addressed table sized `max_rooms`. Codes are not
reused while a room is live.

## State

```c
typedef struct {
    char name[PLAYER_NAME_MAX];
    RoomRef room;              // null ref when lobby-side
    uint8_t seat;
    uint32_t generation;
    bool present;
} Player;

typedef struct {
    RoomCode code;
    PlayerRef seats[ROOM_SEAT_MAX];
    uint8_t seat_count;
    PlayerRef host;
    RoomStatus status;         // LOBBY / RUNNING / ENDED
    RoomConfig config;
    GameRoom game;             // seat_count States, pending inputs, garbage
    uint32_t generation;
    uint64_t frame;
} Room;
```

`GameRoom` holds one `State` per seat (~1.7 KB each), a
`bool[PLAYER_INPUT_KEY_COUNT]` of inputs accumulated since the last frame, and a
pending garbage counter per seat. `max_rooms` (already a config directive) and a
new `room_seat_max` bound the whole allocation, which happens once in
`AppData_init`; rooms are a `SparseSet_Room` and are only ever activated and
deactivated, never resized.

Per-fd `AppEntry` becomes `{ PlayerRef self; }` — the layer's fd-keyed state is
just the handle into the world.

## Effects

`world` never writes a response. It appends to a per-tick scratch vector:

```c
typedef enum { APP_EFFECT_REPLY, APP_EFFECT_EVENT } AppEffectKind;

typedef struct {
    AppEffectKind kind;
    PlayerRef target;
    HtttpStatus status;        // REPLY only
    AppEvent event;            // EVENT only, a tagged union, no heap pointers
} AppEffect;
```

`AppData_flush` walks that vector once, resolves `target` to an fd (dropping
effects whose target is gone or already in `err_fds`), encodes via `event.c` /
`command.c`, and pushes into `m_response_qs`. One place resolves handles to
fds; one place allocates; one place implements the overflow policy below.

## Tick order

```
poll -> accept -> read -> handshake/decrypt -> parse
     -> AppData_respond   (drain pending events, decode, apply, collect effects)
     -> AppData_tick      (step rooms whose frames are due, collect effects)
     -> AppData_flush     (effects -> m_response_qs)
     -> serialize -> encrypt -> write -> sync interest -> close -> resets
```

Commands land before the frame step, so an input arriving this tick affects this
tick's frame rather than the next one.

## Timing

One process-wide `timerfd` at the game frame rate, registered as
`EPOLL_ENTRY_ROOM_TIMERFD` (the enumerator already exists in `epoll.h`), not one
timer per room: room count is bounded by `max_rooms` but fd count is bounded by
`max_fds`, and per-room timers would burn the latter to express the former.

`AppData_tick` reads the expiry count, clamps it (a descheduled daemon must not
try to catch up a hundred frames in one tick), and steps every `RUNNING` room
that many frames. Full-state broadcast runs on a divisor of the frame rate, not
every frame; `state_broadcast_divisor` is a config directive.

## Inputs

`apply_player_inputs` takes a per-frame edge-triggered key array, while clients
send discrete actions over the network. Each seat accumulates a key mask as
commands arrive; the frame step passes the mask to `libtetrisbrain` and clears
it. Two presses of the same key inside one frame collapse into one — the
alternative (queueing presses and applying at most one per frame) drifts a
client's input stream behind wall-clock time under burst, which is worse.

Garbage uses `get_sending_garbage` on the clearing seat, credits the target
seat's pending counter, and applies through `queue_garbage`/`receive_garbage` at
the next frame boundary of the receiving seat, so both seats advance on their
own frames without a cross-seat ordering rule.

## Close and its one-tick lag

The close fan-out runs after `write`, so events generated by a disconnect
(`player left`, `host transferred`, `room destroyed`) cannot reach the socket in
the tick that observes the disconnect. `AppData_close` therefore mutates the
world and appends its effects to a *persistent* pending-event queue rather than
to `m_response_qs`, which is being reset. `AppData_respond` drains that queue
first thing next tick. The lag is one tick; the alternative — running the app's
close handling before `write` — is not available, because `write` itself can add
fds to the close set.

## Overflow policy

Fan-out breaks the capacity contract that every other stage relies on ("at most
one output frame per input frame, so a queue sized `client_capacity` cannot
overflow"). A seat in a running room receives roughly one broadcast per
`state_broadcast_divisor` frames plus one event per action of every other seat,
none of it paced by that seat's own reads, and writes only drain when the poller
reports the socket writable. So the response queue for a player is sized from a
new `outbound_capacity` directive, not from `client_capacity`, and a full queue
is a normal condition rather than an assertion failure:

* Full-snapshot events (`STATE`) are coalescable — on a full queue the oldest
  unsent `STATE` for that fd is replaced, and nothing is lost that the next
  snapshot does not carry.
* Everything else is a delta. On a full queue the connection is marked in
  `err_fds` and closed; a client that cannot keep up with membership changes
  cannot be brought back into sync cheaply.

This is the only place in the pipeline where a slow reader is detected, so the
policy lives in `AppData_flush` and nowhere else.

## Wire mapping

**Superseded by the spec.** This section originally proposed REST-shaped paths
with JSON bodies. The spec fixes a method table instead — `JOIN`, `LEAVE`,
`START`, `MOVE`, `ROTATE`, `DROP`, `STATE` — and fixes their paths and bodies,
so that is what is implemented; see the protocol table in the README. Only the
gaps the spec leaves are ours: `SET_PLAYER_NAME` and `WHOAMI` on `/player`,
and `HOLD` alongside the other play methods.

The status codes are unchanged from what this section said: `201` for room
creation, `409` for join-while-in-a-room and for start-while-running, `403`
for a non-host `START` or a `Player-Id` naming someone else, `404` for an
unknown room, `400` for a malformed body.

Server-initiated events go out as HTTTP *requests* (`EVENT /room/state`,
`EVENT /room/members`, `EVENT /game/over`), not as responses. `HtttpMessage`
already carries `is_request`, and the alternative — unsolicited responses —
destroys the client's ability to pair a response with the request it sent, which
`tetrisu`'s REPL needs. `tetrisu` gets one branch on `is_request` at the top of
its receive path.

## Error handling

Unchanged from `layers.md`: a bad command is an in-band `4xx` reply and the
connection stays open; only an operation failure (allocation, or a violated
capacity precondition) marks `err_fds`. The world's own rejections — wrong room,
not host, room full, game not running — are all replies, never closes.

## Testability

`world` and `game` link nothing but `tetrisbrain` and take an effect sink by
pointer, so a room's whole lifecycle is drivable from a test with no sockets.
That is the reason for the split; it is worth keeping the layer shell free of
rules for that alone.

## Order of work

1. ~~`PlayerRef`/`World` skeleton, `set-name`/`whoami`, replies only, no rooms.~~ done
2. ~~Rooms and membership events — first fan-out, first use of `AppData_flush`.~~ done
3. ~~Timerfd, `RUNNING` rooms, `STATE` broadcast of a placeholder payload.~~ done
4. ~~`libtetrisbrain` behind the frame step; inputs and garbage.~~ done, except that
   garbage is routed round-robin between seats of one room; cross-room transfer
   is battle royale's problem.
5. Host enforcement, room destruction, host transfer. Partly done: `START` is
   host-only and a room is destroyed when its last seat leaves, but host
   transfer is implicit (seat 0 inherits) rather than a command.

Two departures from the plan above are worth recording.

**Membership events.** Step 2 called for membership deltas fanned out to every
member. They are not sent: the roster travels inside every `STATE` snapshot
instead, and snapshots go to lobby rooms as well as running ones. That keeps
`STATE` the only server-originated message, which is what the spec says, and it
removes the whole class of "the client missed a delta and is now out of sync"
problems at the cost of a slightly larger snapshot.

**The close lag.** The one-tick-lag section below describes a persistent
pending-event queue for effects produced during the close fan-out. It is not
needed for the same reason: `world_close` emits nothing, because the departure
is visible in the next snapshot anyway.

**Effect bodies.** `AppEffect` carries its body as a range in a per-tick arena
rather than inline, so an effect stays small enough to hold thousands of them
while a `STATE` body can still be a kilobyte.
