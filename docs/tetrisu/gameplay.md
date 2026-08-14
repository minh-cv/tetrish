# `tetrisu` gameplay client

## Commands

The reactor accepts these commands in its command editor:

| command | HTTTP request | completion |
|---|---|---|
| `create [seats] [public] [cross]` | `CREATE /`, body `{"max_players":N,...}` when any option is given | response (`201`, or an error status) |
| `join <room-id>` | `JOIN /room/<id>` | response (`200`, or an error status) |
| `start` | `START /` | response (`200`, or an error status) |
| `move left`, `move right` | `MOVE /`, body `LEFT` or `RIGHT` | encrypted frame written |
| `rotate cw`, `rotate ccw` | `ROTATE /`, body `CW` or `CCW` | encrypted frame written |
| `drop soft`, `drop hard` | `DROP /`, body `SOFT` or `HARD` | encrypted frame written |
| `hold` | `HOLD /`, empty body | encrypted frame written |
| `leave` | `LEAVE /` | response (`200`, or an error status) |

A bare `create` sends no body and takes the server's room defaults, which seat
one player. Options are matched by shape rather than by a flag grammar: a
decimal word is the seat count, and `public` and `cross` name flags. `join`
requires exactly one all-digit room id, refused locally rather than sent.

`CREATE` and `JOIN` answer with the room id as their body, which the client
keeps and shows in the status line — that code is what another player passes
to `join`.

`CREATE`, `JOIN`, `START`, and `LEAVE` have at most one response in flight.
Gameplay inputs are one-way: waiting for a response to one is a protocol bug,
because `tetrisd` deliberately emits none.

## Reactor data flow

1. `tui.h` drains stdin once, without blocking.
2. The terminal adapter turns current-frame text or game keys into typed
   `GameIntentType` values.
3. The reducer validates the current room/game phase and emits an owned send
   effect. Inputs arriving while a frame is being sent enter a bounded FIFO.
4. The effect runner maps the intent to `ClientRequest` and submits it to the
   single-request `NetClient`.
5. The network reducer sends the next queued input after `SEND_COMPLETED` or a
   terminal response.

The FIFO holds 32 one-way inputs. When full, the newest input is rejected with
a status notification. Disconnect, successful leave, or an inactive `STATE`
clears it.

## Server push

The only accepted unsolicited message is:

```text
STATE /room/<decimal-room-id>
Content-Type: application/tetris-state
Content-Length: 452
```

The network boundary validates the route and decodes the fixed-width body with
`proto_parse_state_request`. A malformed route or body is a protocol error and
closes the connection. A valid `STATE` can arrive while the client is idle,
sending, or awaiting a lifecycle response; it never completes that request.

The reducer keeps a value-copy `ProtoStateRequest`, and the TUI renders its
visible board, active and ghost pieces, hold, next queue, score, combo, B2B,
and garbage balance.

## Input modes

Outside an active game, keyboard input edits the command line. During a game,
Enter after a command switches to game mode; `:` or Escape toggles back to the
command editor.

| game key | intent |
|---|---|
| Left or `a` | move left |
| Right or `d` | move right |
| Down or `s` | soft drop |
| Up or `x` | rotate clockwise |
| `z` | rotate counter-clockwise |
| Space | hard drop |
| `c` | hold |
| Ctrl-C | quit and restore the terminal |

## Tests

`tetrisu_unit_tests` covers command routing, exact protocol tokens, STATE
decoding, response-driven room transitions, input queuing, and STATE delivery
while a send is pending.

When both daemons are built in foreground development mode,
`tetrisu_gameplay_system` generates temporary credentials, starts real
`tetrislogd` and `tetrisd` processes, completes the secure handshake, and runs
the lifecycle plus every gameplay method through the real non-blocking client.
