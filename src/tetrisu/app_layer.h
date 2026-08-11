#ifndef TETRISH_TETRISU_APP_LAYER_H
#define TETRISH_TETRISU_APP_LAYER_H

#include "htttp_layer.h"
#include "input_layer.h"
#include "tetrisbrain.h"
#include "type.h"
#include <stdint.h>

#define CLIENT_NAME_MAX 64

typedef enum {
    MODE_SHELL,
    MODE_GAME,
} AppMode;

/*!
    @brief What the client expects back, so a response can be acted on rather
    than merely printed.

    HTTTP as implemented carries no request identifier, so responses are
    matched to requests FIFO. That is sound only because the client issues one
    command at a time and the server answers in order; a command that ever
    fans out into several concurrent requests would need a request-id header
    and a keyed queue here.
*/
typedef enum {
    PENDING_GENERIC,      // `htttp` — print the response verbatim
    PENDING_SET_NAME,
    PENDING_WHOAMI,
} PendingKind;

typedef struct {
    PendingKind kind;
    char name[CLIENT_NAME_MAX];   // SET_NAME only: what was asked for
} PendingRequest;

#define RING_BUFFER_ELEM_TYPE PendingRequest
#define RING_BUFFER_TYPEDEF PendingRequestQueue
#include "collection/ring_buffer.h"

typedef struct {
    char* text;   // owned, NUL-terminated
} TranscriptLine;

#define RING_BUFFER_ELEM_TYPE TranscriptLine
#define RING_BUFFER_TYPEDEF TranscriptQueue
#include "collection/ring_buffer.h"

/*!
    @brief Everything the UI layer may read. The application layer writes it;
    the UI layer only reads it, so a frontend can be replaced without touching
    application code.
*/
typedef struct {
    AppMode mode;
    char player_name[CLIENT_NAME_MAX];
    long player_id;               // -1 until a response told us
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
    @post per-tick scratch is reclaimed; transcript entries produced this tick
          are retained until the UI has drawn them
*/
void AppData_reset(AppData* data);

/*!
    @brief one application pass over this tick's inputs

    Three jobs in a fixed order, so the ordering is visible rather than
    emergent: route what arrived from the server, handle what the user typed,
    then emit whatever the frame tick owes.

    @pre  no fault is set
    @post @c view is fully updated before the UI layer runs
    @post @c requested_mode may differ from @c view.mode ; client_apply_mode
          performs the switch at tick end
    @post EOF, the `quit` command, and an unrecoverable server push all set
          @c quit
*/
void AppData_step(AppData* data, const InputEventQueue* m_event_q,
                  const HtttpParsedMessageQueue* m_parsed_q,
                  HtttpOutboundMessageQueue* m_request_q,
                  bool frame_tick, ClientFault* fault);

/*!
    @brief append a line to the transcript, taking a printf-style format

    Used by the composition root for connection-level notices, so they land in
    the same place as everything else the user sees.
*/
void AppData_note(AppData* data, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
