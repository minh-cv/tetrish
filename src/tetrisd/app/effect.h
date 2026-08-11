#ifndef TETRISH_TETRISD_APP_EFFECT_H
#define TETRISH_TETRISD_APP_EFFECT_H

#include "htttp.h"
#include "app/ref.h"
#include <stdbool.h>
#include <stddef.h>

/*!
    @brief What the world wants sent, named by handle rather than by fd.

    A reply answers the request being applied; an event is unsolicited and is
    serialized as an HTTTP *request* so the client can still pair its own
    responses with the requests it issued. Both carry their body as a range in
    the sink's arena rather than a pointer, so the world allocates nothing and
    an effect stays trivially copyable.
*/
typedef enum {
    APP_EFFECT_REPLY,
    APP_EFFECT_EVENT,
} AppEffectKind;

typedef struct {
    AppEffectKind kind;
    PlayerRef target;
    HtttpStatus status;     // REPLY only
    const char* method;     // EVENT only, a string literal
    size_t path_offset;     // EVENT only, index into AppEffectSink.arena
    size_t path_len;
    size_t body_offset;     // index into AppEffectSink.arena
    size_t body_len;
    bool coalescable;       // a full snapshot: droppable if the peer is behind
} AppEffect;

/*!
    @brief The per-tick collection point for everything the world emits.

    Both the effect vector and the body arena are allocated once and reused,
    so emitting is bounded work with no allocation. Overflow of either is
    reported to the caller rather than silently truncating.
*/
typedef struct {
    AppEffect* effects;
    size_t count;
    size_t capacity;

    char* arena;
    size_t arena_used;
    size_t arena_capacity;
} AppEffectSink;

int app_effect_sink_init(AppEffectSink* sink, size_t capacity, size_t arena_capacity);
void app_effect_sink_free(AppEffectSink* sink);

/*!
    @post @c count and @c arena_used are 0; the storage is retained
*/
void app_effect_sink_reset(AppEffectSink* sink);

/*!
    @brief copy @p body into the arena and append a reply effect

    @return -1 if either the effect vector or the arena is full, leaving the
            sink exactly as it was
*/
int app_effect_reply(AppEffectSink* sink, PlayerRef target, HtttpStatus status,
                     const char* body, size_t body_len);

/*!
    @brief copy @p path and @p body into the arena and append an event effect

    @param method a string literal naming the server-originated method
    @param coalescable true for a full snapshot, which AppData_flush may drop
           in favour of a newer one when the peer's queue is full
*/
int app_effect_event(AppEffectSink* sink, PlayerRef target, const char* method,
                     const char* path, size_t path_len,
                     const char* body, size_t body_len, bool coalescable);

/*!
    @brief the body of @p effect

    @return a pointer into the arena, valid until the next sink reset
*/
const char* app_effect_body(const AppEffectSink* sink, const AppEffect* effect);

/*!
    @brief the path of @p effect

    @pre @p effect is an APP_EFFECT_EVENT
    @return a pointer into the arena, valid until the next sink reset
*/
const char* app_effect_path(const AppEffectSink* sink, const AppEffect* effect);

#endif
