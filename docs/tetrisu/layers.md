# Domain layers of `tetrisu`

## Overview

`tetrisu` is the interactive client. It follows the layer conventions of
[`tetrisd`](../tetrisd/layers.md) — per-layer state, a fixed chain of processing functions per tick,
`init`/`accept`/`process`/`reset`/`close`/`free` lifecycle naming, consume-in-place ownership — with
four differences forced by the client's shape.

1. **One connection.** Every `{Fd: [Object]}` collapses to `[Object]`. No `SparseSet` appears in
   `tetrisu`; queues are plain ring buffers held directly by their layer. The error set `{Fd}`
   collapses to a single fault enum on the client.
2. **Event sources other than sockets.** The terminal is an input source and an output device. A
   frame timer drives rendering. The poller therefore multiplexes a heterogeneous, fixed set of
   descriptors instead of a table of peers.
3. **Two frontends.** A line-oriented shell (cooked stdin, `printf` transcript) and a full-screen
   game (raw stdin via `tui`, cell buffer via `tui`/`tuiui`). Both are driven by the same tick; the
   input and UI layers each switch backend on the mode the application layer selects.
4. **Requests flow outbound, responses and pushes flow inbound.** The HTTTP layer is the mirror
   image of the server's: it serializes requests and parses responses, and it must also accept
   inbound *requests* (server pushes such as `STATE`).

The intended scope is `docs/plan.md` step 1 through singleplayer, arranged so battle royale (step 2)
is additive. Where a decision was made for the sake of that, it is called out under
[Battle-royale extension points](#battle-royale-extension-points).

## Assumptions

These are assumptions, not facts read off the spec. If the spec at `pa/tetrish` contradicts one,
that is the side to follow.

* **The server is authoritative, including in singleplayer.** `tetrisd` runs `libtetrisbrain`,
  advances it on its own tick, and pushes board state; `tetrisu` sends inputs and renders what it is
  told. Singleplayer is a room of one. The alternative — the client simulating locally and the
  server only relaying garbage — makes step 1 slightly simpler and step 2 a rewrite, because two
  simulations then have to be reconciled. `tetrisu` still links `tetrisbrain`, but only for its
  types (`State`, `Board`, `TetrominoCellType`) and rendering, not for `apply_*`.
* **Responses are matched to requests FIFO.** HTTTP as implemented carries no request identifier.
  The client keeps one pending-request queue and pops its front for each inbound response. This is
  only sound because the client never has more than the ordered set of requests it issued
  outstanding, and the server answers in order.
* **The handshake runs synchronously at startup.** `tetrish_client_handshake` is blocking and there
  is no client-side counterpart to `tetrisd`'s `AuthData` handshake state machine in corestack. The
  client connects and handshakes on a blocking socket before any UI exists, then sets `O_NONBLOCK`
  and enters the tick loop. See [Gaps in corestack](#gaps-in-corestack).

## Event sources and the tick

| Source | Descriptor | Interest | Owner |
| --- | --- | --- | --- |
| server connection | `Connector.server_fd` | `POLLIN` always; `POLLOUT` iff the write queue is nonempty | `ServerIo` |
| terminal input | `STDIN_FILENO` | `POLLIN`, disabled on EOF | `InputData` |
| frame timer | `UiData.frame_timerfd` | `POLLIN`, armed only in `MODE_GAME` | `UiData` |

Three fixed descriptors do not justify `epoll` plus a registry. The poller is a fixed
`struct pollfd[POLLER_SLOT_COUNT]` with a slot per source and an infinite timeout, so every tick is
still one wakeup driven by readiness, exactly as in `tetrisd`. The timer is a `timerfd` rather than a
poll timeout so that "tick" keeps meaning "something became ready" and no layer has to compute
deadlines.

One tick, in order:

```
Poller_poll
InputData_read          stdin bytes      -> [InputEvent]
ServerIo_read           socket bytes     -> [ReaderFrame]
AuthData_decrypt        [ReaderFrame]    -> [AuthFrame]
HtttpData_parse         [AuthFrame]      -> [HtttpParsedMessage]
AppData_step            [InputEvent] + [HtttpParsedMessage] -> [HtttpOutboundMessage] + ViewModel
HtttpData_serialize     [HtttpOutboundMessage] -> [WriterFrame]
AuthData_encrypt        [WriterFrame]    -> [WriterFrame]
ServerIo_write          [WriterFrame]    -> socket
UiData_render           ViewModel        -> terminal
Poller_sync_interest
client_apply_mode       mode switch, if the application layer requested one
*_reset
```

Input is read before the socket so that a keystroke and the frames that arrive in the same wakeup are
seen by one `AppData_step`, and the request that keystroke produces goes out on the same tick.
Rendering runs after the application layer so a frame reflects everything that happened in the tick,
and after the write so a slow terminal never delays the socket.

## Shared types (`src/tetrisu/type.h`)

The macro-template collections must be instantiated once per translation unit, so instantiations
shared between layers live here, exactly as in `tetrisd`. `FrameStatus`, `AuthFrame` and
`HtttpOutboundMessage` are byte-for-byte what `src/tetrisd/type.h` declares; they are duplicated for
now, and are the obvious candidates to lift into corestack once both sides are stable.

```c
#ifndef TETRISH_TETRISU_TYPE_H
#define TETRISH_TETRISU_TYPE_H

#include "htttp.h"          // IWYU pragma: keep
#include "network/reader.h" // IWYU pragma: keep
#include "network/writer.h" // IWYU pragma: keep

typedef enum {
    FRAME_OK,
    FRAME_DECRYPT_ERROR,
    FRAME_PAYLOAD_TOO_LARGE,
    FRAME_HTTTP_PARSE_ERROR,
} FrameStatus;

typedef struct {
    ReaderFrameContent frame;
    FrameStatus status;
} AuthFrame;

#define RING_BUFFER_ELEM_TYPE AuthFrame
#define RING_BUFFER_TYPEDEF AuthFrameQueue
#include "collection/ring_buffer.h"

typedef struct {
    HtttpMessage message;
    HtttpMessageOwnership ownership;
} HtttpOutboundMessage;

#define RING_BUFFER_ELEM_TYPE HtttpOutboundMessage
#define RING_BUFFER_TYPEDEF HtttpOutboundMessageQueue
#include "collection/ring_buffer.h"

/*!
    @brief The single-connection replacement for tetrisd's error set. A layer
    that cannot commit its pass sets a fault; every later layer in the tick
    skips its work while a fault is set, and the tick ends by tearing the
    connection down.
*/
typedef enum {
    FAULT_NONE,
    FAULT_TRANSPORT,  // socket error, EOF, handshake or crypto failure
    FAULT_PROTOCOL,   // the peer sent something the client cannot honour
    FAULT_LOCAL,      // allocation failure, terminal failure
} ClientFault;

#endif
```

## Layers

### `Connector` (`connector.h`)

The client's counterpart to `Acceptor`: it produces the one descriptor everything else is about, and
owns it. Resolution, `connect`, and `tetrish_client_handshake` all happen inside `Connector_init`
against a blocking socket; only afterwards is the descriptor switched to non-blocking and handed to
`ServerIo` and `AuthData`.

```c
typedef struct {
    int server_fd;      // -1 when not connected
    SessionKey key;     // valid iff server_fd >= 0
} Connector;

/*!
    @brief resolve @p address : @p port , connect, and complete the tetrissh
    handshake against @p ca_path , then put the socket in non-blocking mode.

    @post on success @c server_fd is a connected non-blocking socket and
          @c key holds the negotiated session key
    @return -1 on any failure, with no descriptor leaked
*/
int Connector_init(Connector* data, const char* address, int port, const char* ca_path);
void Connector_free(Connector* data);
```

### `Poller` (`poller.h`)

```c
typedef enum {
    POLLER_SLOT_SERVER,
    POLLER_SLOT_INPUT,
    POLLER_SLOT_FRAME_TIMER,
    POLLER_SLOT_COUNT,
} PollerSlot;

typedef struct {
    struct pollfd slots[POLLER_SLOT_COUNT];  // fd < 0 means the slot is unused
    short revents[POLLER_SLOT_COUNT];
} Poller;

void Poller_init(Poller* data);

/*!
    @brief register @p fd in @p slot with interest @p events

    @pre the slot is unused
    @post the slot is readable through @c Poller_ready until Poller_close
*/
void Poller_accept(Poller* data, PollerSlot slot, int fd, short events);
void Poller_close(Poller* data, PollerSlot slot);

/*!
    @brief block until at least one registered source is ready

    @post @c revents holds this tick's readiness; EINTR yields an all-zero
          tick rather than a fault, so a signal just re-enters the loop
    @return -1 on an unrecoverable poll failure
*/
int Poller_poll(Poller* data);

bool Poller_ready(const Poller* data, PollerSlot slot, short events);

/*!
    @brief level-synchronize interest masks at tick end

    @post POLLOUT on POLLER_SLOT_SERVER is armed iff @p write_pending
    @post POLLIN on POLLER_SLOT_INPUT is disarmed iff @p input_eof
*/
void Poller_sync_interest(Poller* data, bool write_pending, bool input_eof);

void Poller_reset(Poller* data);
```

### `ServerIo` (`server_io.h`)

`PlayerIo` with the sparse sets removed. It owns the corestack `Reader`/`Writer` pair and the two
frame queues either side of them, and it is the only layer that touches the socket.

```c
typedef struct {
    Reader reader;
    Writer writer;
    ReaderFrameQueue read_q;
    WriterFrameQueue write_q;
    bool write_pending;    // write_q nonempty or a partial frame is in flight
} ServerIo;

int ServerIo_init(ServerIo* data, size_t queue_capacity);
void ServerIo_free(ServerIo* data);

/*!
    @post all frames in @c read_q are freed; @c write_q is untouched, since
          pending writes survive ticks until the socket is writable
*/
void ServerIo_reset(ServerIo* data);

/*!
    @brief one read pass, appending complete frames to @p m_read_q

    @post a socket error or a clean EOF sets @p fault to FAULT_TRANSPORT
    @post an oversized frame is in-band: it arrives with a non-OK
          ReaderFrameStatus rather than faulting the connection
*/
void ServerIo_read(ServerIo* data, int fd, ReaderFrameQueue* m_read_q, ClientFault* fault);

/*!
    @brief drain @p m_write_q to the socket as far as it accepts

    @post fully flushed frames are popped and freed; a socket error sets
          @p fault to FAULT_TRANSPORT
    @post @c write_pending reflects the queue level for Poller_sync_interest
*/
void ServerIo_write(ServerIo* data, int fd, WriterFrameQueue* m_write_q, ClientFault* fault);
```

### `AuthData` (`auth.h`)

Post-handshake only: the session key arrives from `Connector`, so this layer is `tetrisd`'s auth
minus the handshake branch and minus the credential. Keeping the layer (rather than folding
`tetrish_session_encrypt`/`_decrypt` into `ServerIo`) is what leaves room for the non-blocking client
handshake described under [Gaps in corestack](#gaps-in-corestack).

```c
typedef struct {
    SessionKey key;
    AuthFrameQueue decrypt_q;    // inbound plaintext, owned here
    WriterFrameQueue encrypt_q;  // outbound plaintext staged by the HTTTP layer
} AuthData;

int AuthData_init(AuthData* data, const SessionKey* key, size_t queue_capacity);
void AuthData_free(AuthData* data);
void AuthData_reset(AuthData* data);

/*!
    @brief decrypt every frame in @p m_read_q into @p m_decrypt_q

    @post a frame whose ReaderFrameStatus is not OK is forwarded with the
          matching FrameStatus and is not decrypted
    @post a decryption failure is in-band as FRAME_DECRYPT_ERROR, not a fault:
          the application layer decides whether to close
    @post an allocation failure sets @p fault to FAULT_LOCAL
*/
void AuthData_decrypt(AuthData* data, const ReaderFrameQueue* m_read_q,
                      AuthFrameQueue* m_decrypt_q, ClientFault* fault);

/*!
    @brief encrypt every frame in @p m_encrypt_q into @p m_write_q

    @post frames appended to @p m_write_q are owned by ServerIo, which frees
          them once flushed
    @post any failure sets @p fault; nothing partial is left in @p m_write_q
*/
void AuthData_encrypt(AuthData* data, const WriterFrameQueue* m_encrypt_q,
                      WriterFrameQueue* m_write_q, ClientFault* fault);
```

### `HtttpData` (`htttp_layer.h`)

The mirror of the server's. `HtttpParsedMessage` is the same non-owning view into the decrypted
frame; the difference is that a parsed message here is usually a response, and the application layer
must handle both.

```c
typedef struct {
    HtttpMessage message;   // non-owning view into the decrypt_q frame
    FrameStatus status;     // message is zeroed unless status is FRAME_OK
} HtttpParsedMessage;

#define RING_BUFFER_ELEM_TYPE HtttpParsedMessage
#define RING_BUFFER_TYPEDEF HtttpParsedMessageQueue
#include "collection/ring_buffer.h"

typedef struct {
    HtttpParsedMessageQueue parsed_q;
} HtttpData;

int HtttpData_init(HtttpData* data, size_t queue_capacity);
void HtttpData_free(HtttpData* data);
void HtttpData_reset(HtttpData* data);

/*!
    @brief parse every frame of @p m_decrypt_q in place into @p m_parsed_q

    @pre  @p m_decrypt_q and @p m_parsed_q have the same capacity, so the
          one-message-per-frame output always fits
    @post a malformed message travels in-band as FRAME_HTTTP_PARSE_ERROR
    @post frame contents may be modified in place; ownership does not change
*/
void HtttpData_parse(HtttpData* data, const AuthFrameQueue* m_decrypt_q,
                     HtttpParsedMessageQueue* m_parsed_q, ClientFault* fault);

/*!
    @brief serialize every message of @p m_request_q into @p m_encrypt_q

    @post a serialization failure, an empty result, or a result over FRAME_MAX
          sets @p fault to FAULT_LOCAL
*/
void HtttpData_serialize(HtttpData* data, const HtttpOutboundMessageQueue* m_request_q,
                         WriterFrameQueue* m_encrypt_q, ClientFault* fault);
```

### `InputData` (`input_layer.h`)

Owns `STDIN_FILENO` and its terminal discipline, and turns bytes into events the application layer
can consume without knowing which frontend is up. Both backends produce the same
`InputEvent`, so the application layer's command handling is written once.

* `INPUT_MODE_LINE` — cooked stdin. `InputData_read` reads what is available, assembles complete
  lines, and tokenizes each with `cmdline_split` (below), emitting one `INPUT_EVENT_COMMAND`.
* `INPUT_MODE_RAW` — `tui` has stdin in raw non-blocking mode. `InputData_read` calls
  `tui_poll_events` and copies `tui_input_events()` into the queue as `INPUT_EVENT_KEY`.

```c
typedef enum {
    INPUT_EVENT_COMMAND,   // a tokenized line (line mode)
    INPUT_EVENT_KEY,       // one decoded key (raw mode)
    INPUT_EVENT_EOF,       // stdin closed
} InputEventType;

#define CMDLINE_MAX_ARGS 64

typedef struct {
    InputEventType type;
    struct { char* argv[CMDLINE_MAX_ARGS]; size_t argc; } command;  // owned by the event
    struct { TuiKey key; unsigned char ch; uint8_t modifiers; } key;
} InputEvent;

#define RING_BUFFER_ELEM_TYPE InputEvent
#define RING_BUFFER_TYPEDEF InputEventQueue
#include "collection/ring_buffer.h"

typedef enum { INPUT_MODE_LINE, INPUT_MODE_RAW } InputMode;

typedef struct {
    InputMode mode;
    InputEventQueue event_q;
    char* line_buf;          // partial line carried across ticks in line mode
    size_t line_used;
    size_t line_capacity;
    bool eof;
} InputData;

int InputData_init(InputData* data, size_t queue_capacity, size_t line_capacity);
void InputData_free(InputData* data);

/*!
    @post every event in @c event_q is freed, including command argv storage
*/
void InputData_reset(InputData* data);

/*!
    @brief read what stdin has available and append events to @p m_event_q

    @post EOF appends INPUT_EVENT_EOF exactly once and latches @c eof
    @post a line longer than @c line_capacity is discarded up to the next
          newline with a diagnostic event; it is not a fault
*/
void InputData_read(InputData* data, InputEventQueue* m_event_q, ClientFault* fault);

/*!
    @brief switch terminal discipline

    @pre  called only at tick end, from client_apply_mode
    @post buffered bytes that belong to the old discipline are discarded
          (@c line_buf cleared, or tui_stdin_clear), so no keystroke is
          reinterpreted under the new one
    @post entering INPUT_MODE_RAW requires tui_init to have succeeded
*/
int InputData_set_mode(InputData* data, InputMode mode);
```

`cmdline_split` is the tokenizer from `docs/plan.md` step 1, in its own translation unit
(`cmdline.h`/`cmdline.c`) precisely because the plan calls for `tetrish` to reuse it:

```c
/*!
    @brief split @p line into argv, honouring '...' (literal) and "..."
           (escapes processed) quoting

    @post on success @p out_argv holds @p out_argc heap strings the caller
          frees with cmdline_free
    @return -1 on unterminated quote, bad escape, allocation failure, or more
            than CMDLINE_MAX_ARGS tokens
*/
int cmdline_split(const char* line, char* out_argv[CMDLINE_MAX_ARGS], size_t* out_argc);
void cmdline_free(char* argv[CMDLINE_MAX_ARGS], size_t argc);
```

### `AppData` (`app_layer.h`)

The client's brain, and the only layer with domain knowledge. It consumes input events and parsed
messages, produces outbound requests and the view model, and owns the mode.

Three distinct jobs, deliberately kept as three functions behind one `AppData_step` so the ordering
is fixed and visible:

1. **Route inbound.** A parsed *response* pops the front of `pending` and runs that request's
   continuation. A parsed *request* is a server push, routed by method through a push table, and
   never touches `pending`. A non-OK `FrameStatus` is reported to the user; only
   `FRAME_DECRYPT_ERROR` faults, because a client that cannot decrypt cannot stay in session.
2. **Handle input.** In `MODE_SHELL`, an `INPUT_EVENT_COMMAND` is dispatched through the command
   table; the handler builds an `HtttpOutboundMessage`, pushes it, and pushes a matching
   `PendingRequest`. In `MODE_GAME`, `INPUT_EVENT_KEY` sets bits in `input_latch` and nothing is
   sent yet.
3. **Emit periodic work.** On a frame tick in `MODE_GAME`, the latch is drained into one input
   request and cleared. Input is therefore sent at the frame rate, not per keystroke, which bounds
   the request rate and matches `apply_player_inputs`' `bool[PLAYER_INPUT_KEY_COUNT]` shape.

```c
typedef enum { MODE_SHELL, MODE_GAME } AppMode;

typedef enum {
    PENDING_GENERIC,      // `htttp` — print the response verbatim
    PENDING_SET_NAME,
    PENDING_WHOAMI,
    PENDING_ENTER_GAME,   // on 2xx, switch to MODE_GAME
    PENDING_LEAVE_GAME,
} PendingKind;

typedef struct { PendingKind kind; } PendingRequest;

#define RING_BUFFER_ELEM_TYPE PendingRequest
#define RING_BUFFER_TYPEDEF PendingRequestQueue
#include "collection/ring_buffer.h"

typedef struct {
    char* text;     // owned
    size_t len;
} TranscriptLine;

#define RING_BUFFER_ELEM_TYPE TranscriptLine
#define RING_BUFFER_TYPEDEF TranscriptQueue
#include "collection/ring_buffer.h"

/*!
    @brief everything the UI layer may read. The application layer writes it;
    the UI layer only reads it, so a frontend can be replaced without touching
    application code.
*/
typedef struct {
    AppMode mode;
    const char* player_name;      // NULL until SET_PLAYER_NAME succeeded
    int player_id;                // -1 until known
    bool connected;

    State board;                  // last STATE push, libtetrisbrain's type
    bool board_valid;
    uint64_t board_seq;           // monotone, so the UI can skip redundant draws

    TranscriptQueue transcript;   // shell lines and diagnostics, newest last
    bool dirty;                   // set on any change, cleared by UiData_render
} ViewModel;

typedef struct {
    ViewModel view;
    HtttpOutboundMessageQueue request_q;
    PendingRequestQueue pending;
    bool input_latch[PLAYER_INPUT_KEY_COUNT];
    AppMode requested_mode;       // == view.mode unless a switch is pending
    bool quit;
} AppData;

int AppData_init(AppData* data, size_t queue_capacity);
void AppData_free(AppData* data);

/*!
    @post request_q is empty (the HTTTP layer consumed it) and transcript
          entries produced this tick are retained; only per-tick scratch is
          reclaimed
*/
void AppData_reset(AppData* data);

/*!
    @brief one application pass over this tick's inputs

    @pre  no fault is set
    @pre  @p m_request_q has capacity for one request per input event plus one
          periodic request

    @post @c view is fully updated before the UI layer runs
    @post @c requested_mode may differ from @c view.mode ; client_apply_mode
          performs the switch at tick end
    @post INPUT_EVENT_EOF, the `quit` command, and an unrecoverable server push
          all set @c quit
*/
void AppData_step(AppData* data, const InputEventQueue* m_event_q,
                  const HtttpParsedMessageQueue* m_parsed_q,
                  HtttpOutboundMessageQueue* m_request_q,
                  bool frame_tick, ClientFault* fault);
```

The command table is data, not a `strcmp` chain, so adding step-2 commands is one row each:

```c
typedef int (*CommandHandler)(AppData*, size_t argc, char* const argv[],
                              HtttpOutboundMessageQueue* m_request_q);

typedef struct {
    const char* name;
    size_t min_args;
    size_t max_args;
    CommandHandler handler;
} CommandEntry;
```

Step 1 rows: `htttp`, `set-name`, `whoami`, `singleplayer`, `quit`. The push table has the same
shape, keyed on the request method of an inbound push: `STATE` in step 1.

### `UiData` (`ui_layer.h`)

Reads the view model and draws it. It owns the frame timer and, in game mode, the `tui` singleton.

* `UI_BACKEND_LINE` — appends new transcript entries to stdout. Renders whenever `view.dirty`.
* `UI_BACKEND_TUI` — clears the cell buffer, draws the board from `view.board`, draws the HUD
  through `tuiui`, and presents. Renders on the frame tick only.

```c
typedef enum { UI_BACKEND_LINE, UI_BACKEND_TUI } UiBackend;

typedef struct {
    UiBackend backend;
    int frame_timerfd;      // -1 unless UI_BACKEND_TUI
    size_t transcript_drawn; // how much of view.transcript has been printed
    bool tui_up;
} UiData;

int UiData_init(UiData* data);

/*!
    @post tui_shutdown has run if it was up, so the terminal is restored even
          on an error path
*/
void UiData_free(UiData* data);

/*!
    @brief draw one frame

    @pre  the application layer has already run this tick
    @post @p m_view->dirty is cleared
    @post in UI_BACKEND_TUI nothing is drawn unless @p frame_tick
*/
void UiData_render(UiData* data, ViewModel* m_view, bool frame_tick, ClientFault* fault);

/*!
    @brief consume the timerfd expiration count

    @pre  the frame timer slot was reported readable
*/
void UiData_read_frame_timer(UiData* data, ClientFault* fault);

/*!
    @brief switch backend

    @pre  called only at tick end, from client_apply_mode, and paired with the
          matching InputData_set_mode
    @post entering UI_BACKEND_TUI calls tui_init and arms the frame timer at
          the configured interval; leaving it disarms and closes the timer and
          calls tui_shutdown
    @post the caller registers or closes the timer's poller slot to match
*/
int UiData_set_backend(UiData* data, UiBackend backend, unsigned frame_interval_ms);
```

### `Client` (`client.h`)

The composition root, mirroring `Server`.

```c
typedef struct {
    struct config_var cfg;
    Connector connector;
    Poller poller;
    ServerIo io;
    AuthData auth;
    HtttpData htttp;
    InputData input;
    AppData app;
    UiData ui;
    ClientFault fault;
} Client;

int client_init(Client* client);
void client_free(Client* client);

/*!
    @brief one tick, in the order listed under "Event sources and the tick"

    @post returns false once the application asked to quit or a fault was
          raised and the teardown ran
*/
bool client_tick(Client* client);
```

`main` is then the shape `tetrisd`'s already has: install signal handlers (`SIGINT`/`SIGTERM` set
`running`, `SIGPIPE` ignored), `client_init`, `while (running && client_tick(&client))`,
`client_free`.

## Ownership and consumption

Unchanged from `tetrisd`, and worth restating for the two places the client differs:

* A pass consumes its input by reading in place, never by popping; reclamation happens at that
  layer's `reset`. `HtttpParsedMessage` holds non-owning views into `AuthData.decrypt_q` frames, so
  `AuthData_reset` must run no earlier than `HtttpData_reset`.
* Writing is the exception: `ServerIo_write` pops and frees frames as they are flushed.
* `InputEvent.command.argv` is heap storage owned by the event and freed at `InputData_reset`. The
  application layer may keep a token beyond the tick only by copying it — a player name stored in the
  view model is a copy.
* `ViewModel.transcript` and `ViewModel.board` outlive the tick by design; they are the only
  application state the UI reads, and are freed at `AppData_free`.

## Error handling

* `ClientFault` replaces the server's error set. A layer that cannot commit its pass discards its
  staged output, undoes its activation, and sets the fault; every later layer in the tick checks and
  skips.
* Recoverable per-frame conditions stay in-band exactly as on the server: an oversized frame or a
  malformed message reaches the application layer as a status, and the application layer reports it
  in the transcript without closing. The asymmetry with `tetrisd` is that a decrypt failure is fatal
  here — the server answers a bad frame with a 400 and keeps the peer, but a client that cannot
  decrypt has lost session sync and has nothing useful to say.
* A fault ends the tick with teardown: `ServerIo`, `AuthData` and `HtttpData` are closed, the reason
  is printed after the terminal is restored, and `client_tick` returns false. Reconnect is an
  extension point, not step-1 behaviour.
* `UiData_free` must be reachable from every exit path, including a fault raised while the `tui` is
  up, or the user is left in raw mode on the alternate screen.

## Mode switching

Mode is one field, owned by `AppData`, decided during `AppData_step` as `requested_mode`, and applied
by `client_apply_mode` at tick end. The switch is deferred rather than immediate for one reason: the
input layer's stdin discipline and the UI layer's terminal state must change together, and they
cannot change while the tick still holds bytes read under the old discipline or a view model already
half-rendered. `client_apply_mode` therefore runs after `UiData_render` and before the resets, and
performs, in order, `UiData_set_backend`, `InputData_set_mode`, and the poller registration for the
frame timer. If either half fails, the client sets `FAULT_LOCAL` rather than continuing with a
half-switched terminal.

## Battle-royale extension points

Every one of these is additive; none changes an existing signature.

* **Board array.** `ViewModel.board`/`board_valid`/`board_seq` become a fixed array indexed by player
  slot, plus the local slot index. The TUI backend gains a multi-board layout; the routing of the
  `STATE` push gains a slot field.
* **Push table.** Room lifecycle pushes (`ROOM_UPDATE`, `START`, `ATTACK`, `ELIMINATED`) are rows in
  the same table `STATE` already uses. Because pushes never touch `pending`, no correlation work is
  needed.
* **Command table.** `create`, `join`, `leave`, `start`, and the paging commands are rows with new
  `PendingKind` values. `singleplayer` stays a row and becomes shorthand for a one-player room.
* **Screens.** `AppMode` grows `MODE_ROOM_LOBBY` and `MODE_ROOM_LIST`. Both use the raw input backend
  and the TUI UI backend, so the mode-switch machinery is unchanged; only the draw function branches.
* **Garbage IPC and chat** are more push rows and more transcript entries.

The parts that are *not* free: FIFO response correlation holds only while the client issues requests
one command at a time. If a step-2 command ever fans out into several concurrent requests, `pending`
has to become keyed, which means a request-id header and a matching change on the server.

## Gaps in corestack

* **No client-side non-blocking handshake.** `tetrish_client_handshake` blocks. It is fine at
  startup and unusable for in-session reconnect, which needs a client counterpart to `tetrisd`'s
  `AuthData` state machine — `AuthData` here is shaped to receive it (a state enum plus a handshake
  output path into `write_q`) without disturbing its callers.
* **`tui` has no resize handling.** `tui_init` takes a fixed `TgSizei` and nothing reads
  `TIOCGWINSZ` or handles `SIGWINCH`, so a resized terminal leaves the game drawing at the old size.
  The client can work around it by tearing down and re-initing the `tui` on a `SIGWINCH` flag at tick
  end, which is exactly the mode-switch path; a real fix belongs in corestack.
* **`tui` key-down expiry is time-based** (180 ms without a repeat). That interacts with the frame
  rate: at a slow frame tick, held-key movement will feel dropped. The latch-and-drain design above
  reads `clicked`-style events rather than `down` state to reduce the exposure, but a held soft-drop
  will need attention.

## Build

```cmake
add_executable(tetrisu src/tetrisu/main.c src/tetrisu/client.c src/tetrisu/config_var.c
    src/tetrisu/connector.c src/tetrisu/poller.c src/tetrisu/server_io.c src/tetrisu/auth.c
    src/tetrisu/htttp_layer.c src/tetrisu/cmdline.c src/tetrisu/input_layer.c
    src/tetrisu/app_layer.c src/tetrisu/ui_layer.c)
target_include_directories(tetrisu PRIVATE src/tetrisu include)
target_link_libraries(tetrisu PRIVATE network tetrissh htttp config common tui tuiui tetrisbrain)
```

`cjson` stays only if the wire bodies are JSON; the step-1 `set-name`/`whoami` bodies do not need it,
and `STATE` bodies are the decision point — a fixed binary board encoding is cheaper to parse per
frame than JSON, and is the one place the spec should be checked before writing code.
