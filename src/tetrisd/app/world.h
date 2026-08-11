#ifndef TETRISH_TETRISD_APP_WORLD_H
#define TETRISH_TETRISD_APP_WORLD_H

#include "app/command.h"
#include "app/effect.h"
#include "app/ref.h"
#include <stddef.h>

#define PLAYER_NAME_MAX 24

/*!
    @brief One connected player. Identity only for now; room membership and
    seat land here when rooms do.

    @c generation is odd while the slot is live and is bumped on release, so a
    handle taken before a disconnect never resolves onto the connection that
    inherited the fd number.
*/
typedef struct {
    char name[PLAYER_NAME_MAX];
    uint32_t generation;
    bool present;
} Player;

/*!
    @brief The rules, with no knowledge of fds, sockets or HTTTP.

    Every command is applied through @c world_apply, which reads the world and
    appends effects to a sink; nothing is sent from here. That is what makes a
    player's whole lifecycle drivable from a test with no server around it.

    @invariant a player slot's index is the fd number it was accepted with,
    which is why @c player_capacity is the fd table size
*/
typedef struct {
    Player* players;
    size_t player_capacity;
} World;

int world_init(World* world, size_t player_capacity);
void world_free(World* world);

/*!
    @brief bring the slot at @p index to life with a default name

    @pre @p index is below @c player_capacity and its slot is not live
    @return a handle to the new player
*/
PlayerRef world_accept(World* world, size_t index);

/*!
    @brief release @p ref's slot, bumping its generation

    @note a stale or null handle is ignored
*/
void world_close(World* world, PlayerRef ref);

/*!
    @brief resolve @p ref

    @return NULL if @p ref is null, out of range, released, or from an earlier
            occupant of the slot
*/
Player* world_player(const World* world, PlayerRef ref);

/*!
    @brief apply @p command on behalf of @p actor, appending what it produces
           to @p sink

    Rejections the rules make — an unknown room, a name that is not usable —
    are replies, never failures: the connection stays open and the caller has
    nothing to handle. A rejected command emits exactly one reply, as does an
    accepted one.

    @pre @p actor resolves
    @return -1 only if @p sink could not take the effects, in which case the
            command had no observable result
*/
int world_apply(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink);

#endif
