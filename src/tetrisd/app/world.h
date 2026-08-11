#ifndef TETRISH_TETRISD_APP_WORLD_H
#define TETRISH_TETRISD_APP_WORLD_H

#include "app/command.h"
#include "app/effect.h"
#include "app/ref.h"
#include "tetrisbrain.h"
#include <stddef.h>

#define PLAYER_NAME_MAX 24
#define ROOM_CODE_MAX 16
#define ROOM_SEAT_MAX 8

/*!
    @brief One connected player: identity plus where they are sitting.

    @c generation is bumped every time the slot is released, so a handle a room
    still holds for a departed player never resolves onto the connection that
    inherited the fd number.
*/
typedef struct {
    char name[PLAYER_NAME_MAX];
    RoomRef room;      // null while in the lobby
    uint8_t seat;      // meaningful only while @c room resolves
    uint32_t generation;
    bool present;
} Player;

typedef enum {
    ROOM_LOBBY,
    ROOM_RUNNING,
    ROOM_ENDED,
} RoomStatus;

/*!
    @brief One seat's game.

    @c inputs is a key mask accumulated as commands arrive and cleared by the
    frame step, so two presses of the same key inside one frame collapse into
    one. Queueing presses instead would drift a client's input stream behind
    wall-clock time under burst, which is worse than losing the repeat.
*/
typedef struct {
    State state;
    bool inputs[PLAYER_INPUT_KEY_COUNT];
    bool alive;
} Seat;

typedef struct {
    char code[ROOM_CODE_MAX];
    PlayerRef occupants[ROOM_SEAT_MAX];
    Seat seats[ROOM_SEAT_MAX];
    uint8_t seat_count;
    PlayerRef host;
    RoomStatus status;
    uint64_t frame;
    uint32_t generation;
    bool present;
} Room;

/*!
    @brief The rules, with no knowledge of fds, sockets or HTTTP.

    Every command is applied through @c world_apply and every frame through
    @c world_tick ; both read the world and append effects to a sink, and
    neither sends anything. That is what makes a room's whole lifecycle
    drivable from a test with no server around it.

    Rooms are found by code with a linear scan. @c room_capacity is
    `max_rooms`, a small number by configuration, and the scan happens once per
    JOIN rather than per frame, so an index would cost more in invariants than
    it saves in time.

    @invariant a player slot's index is the fd number it was accepted with,
    which is why @c player_capacity is the fd table size
*/
typedef struct {
    Player* players;
    size_t player_capacity;
    Room* rooms;
    size_t room_capacity;
} World;

int world_init(World* world, size_t player_capacity, size_t room_capacity);
void world_free(World* world);

/*!
    @brief bring the slot at @p index to life with a default name

    @pre @p index is below @c player_capacity and its slot is not live
    @return a handle to the new player
*/
PlayerRef world_accept(World* world, size_t index);

/*!
    @brief release @p ref's slot, bumping its generation

    Nothing is emitted here even though the room's membership just changed.
    The close fan-out runs after the write stage, so an effect recorded here
    could not reach a socket this tick anyway; the roster travels inside the
    next STATE broadcast instead, which is also what keeps STATE the only
    server-originated message the spec allows.

    @post if the player was seated, they are removed from their room first, so
          the room never holds a handle to a released slot
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
    @brief resolve @p ref
*/
Room* world_room(const World* world, RoomRef ref);

/*!
    @brief apply @p command on behalf of @p actor, appending what it produces
           to @p sink

    Rejections the rules make — an unknown room, a full room, a non-host START
    — are replies, never failures: the connection stays open and the caller has
    nothing to handle.

    @pre @p actor resolves
    @return -1 only if @p sink could not take the effects, in which case the
            command's reply was lost; the world may still have changed
*/
int world_apply(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink);

/*!
    @brief advance every running room by @p frames frames

    @param broadcast whether this call should also emit a STATE snapshot to
           every seated player; the caller runs it on a divisor of the frame
           rate rather than every frame
    @post a seat whose piece cannot spawn is marked dead; a room whose last
          live seat dies moves to ROOM_ENDED
    @return -1 if @p sink could not take a snapshot, which drops that snapshot
            only
*/
int world_tick(World* world, uint64_t frames, bool broadcast, AppEffectSink* sink);

#endif
