#ifndef TETRISH_TETRISD_APP_GARBAGE_EVENT_H
#define TETRISH_TETRISD_APP_GARBAGE_EVENT_H

#include <assert.h>
#include <stdint.h>

#define GARBAGE_EVENT_VERSION 1u

/*!
    @brief One attack: a member's line clear, headed for whoever the
           routing picks at delivery time.

    No target is chosen at production, so there is no stale-target state;
    routing happens against the live roster when the event is applied.

    The struct doubles as the wire format of the garbage queue, so its
    fields are fixed-width and its layout padding-free.
*/
typedef struct {
    //! @brief @c GARBAGE_EVENT_VERSION ; anything else is dropped on receive
    uint8_t version;
    //! @brief the source room's @c cross_room_garbage flag
    uint8_t cross_room;
    //! @brief rows to inject
    uint16_t n_lines;
    //! @brief excluded from targeting; log correlation
    int32_t source_fd;
    //! @brief excluded by cross-room routing; log correlation
    uint32_t source_room_idx;
    //! @brief stamped by the queue on send; log correlation
    uint32_t seq;
} GarbageEvent;
static_assert(sizeof(GarbageEvent) == 16, "GarbageEvent is a wire format");

#define RING_BUFFER_ELEM_TYPE GarbageEvent
#define RING_BUFFER_TYPEDEF Vec_GarbageEvent
#include "collection/ring_buffer.h"

#endif
