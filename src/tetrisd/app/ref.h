#ifndef TETRISH_TETRISD_APP_REF_H
#define TETRISH_TETRISD_APP_REF_H

#include <stdbool.h>
#include <stdint.h>

/*!
    @brief A generational handle into the world.

    @c index is the fd number for a player and the room slot for a room, so
    resolution is O(1) with no lookup structure. @c generation is bumped every
    time the slot is released, which is what makes a handle held across a
    disconnect detectable instead of silently addressing whoever inherited the
    fd number. Generation `0` is reserved for the null handle, so a zeroed
    struct is null.
*/
typedef struct {
    uint32_t index;
    uint32_t generation;
} PlayerRef;

typedef struct {
    uint32_t index;
    uint32_t generation;
} RoomRef;

#define APP_REF_NULL_GENERATION 0u

static inline PlayerRef player_ref_null(void) {
    const PlayerRef ref = {0, APP_REF_NULL_GENERATION};
    return ref;
}

static inline RoomRef room_ref_null(void) {
    const RoomRef ref = {0, APP_REF_NULL_GENERATION};
    return ref;
}

static inline bool player_ref_is_null(PlayerRef ref) {
    return ref.generation == APP_REF_NULL_GENERATION;
}

static inline bool room_ref_is_null(RoomRef ref) {
    return ref.generation == APP_REF_NULL_GENERATION;
}

static inline bool player_ref_eq(PlayerRef a, PlayerRef b) {
    return a.index == b.index && a.generation == b.generation;
}

static inline bool room_ref_eq(RoomRef a, RoomRef b) {
    return a.index == b.index && a.generation == b.generation;
}

#endif
