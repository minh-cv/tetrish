#ifndef TETRISH_TETRISD_APP_ROOM_H
#define TETRISH_TETRISD_APP_ROOM_H

#include "app/app_layer.h"
#include "type.h"
#include <stddef.h>

//! @brief what a bare @c CREATE gets: private, singleplayer, default gameplay
RoomConfig room_config_default(void);

//! @brief the seat of @p fd in @p room , or NULL if it has none
RoomMember* room_find_member(Room* room, Fd fd);

/*!
    @brief put @p fd in a fresh room configured by @p config , in
           @c ROOM_LOBBY

    @pre @p fd is a key in @c players
    @pre @p config 's @c max_players is between `1` and
         @c max_players_per_room , or @p config is NULL for
         @c room_config_default()

    @post on success, a fresh key of @c rooms holds a room whose only seat
          is @p fd 's with no game, @p fd 's @c room_idx is that key, and
          the key is no longer in @c free_room_idxs . @p out_room_idx
          receives it.
    @post on failure, nothing changed

    @return -1 if @p fd is already in a room or no room key is free, 0
            otherwise
*/
int room_create(AppData* data, Fd fd, const RoomConfig* config, size_t* out_room_idx);

/*!
    @brief start the game of the room keyed @p room_idx

    Every member is dealt a fresh board. A room configured with
    @c shared_seed deals every board the same piece sequence, the standard
    fairness rule in versus play; otherwise each member's sequence is their
    own.

    @pre @p room_idx is a key in @c rooms

    @post the room is @c ROOM_IN_GAME , every member's board is freshly
          initialized from the room's @c brain config with its first piece
          in play, no member has pending inputs, every member is alive, and
          @p room_idx is in @c in_game_rooms

    @note the boards are seeded from the wall clock and @p room_idx , so a
          game is not reproducible across runs. This is not part of the
          contract; a seed the caller supplies would be.

    @note starting an already started room restarts its boards. This is not
          part of the contract.
*/
void room_start(AppData* data, size_t room_idx);

/*!
    @brief advance the game of the room keyed @p room_idx by one frame

    The frame applies, per living member, every key recorded since the last
    one at once, then clears them, so a key recorded twice between two
    ticks moves that board once. A member whose board tops out is
    eliminated on the spot; the game ends here once nobody is left alive.

    @pre @p room_idx is a key in @c rooms whose room is @c ROOM_IN_GAME

    @post every living member's board has advanced one frame and no member
          has pending inputs
    @post if the game ended, the room is @c ROOM_LOBBY and @p room_idx is
          absent from @c in_game_rooms
*/
void room_tick(AppData* data, size_t room_idx);

/*!
    @brief end the game of the room keyed @p room_idx

    @post the room is @c ROOM_LOBBY , its members' boards are meaningless,
          and @p room_idx is absent from @c in_game_rooms . The members
          keep their seats, so the room can start again.

    @note ending a room that is already in @c ROOM_LOBBY does nothing. This
          is not part of the contract.
*/
void room_end(AppData* data, size_t room_idx);

/*!
    @brief take @p fd out of its room, if it is in one

    Leaving mid-game is elimination: the game goes on without the leaver,
    and ends if nobody alive remains. A room whose last member leaves is
    destroyed.

    @pre @p fd is a key in @c players

    @post @p fd 's @c room_idx is @c ROOM_IDX_NONE and no room seats it
    @post if that emptied the room, its key is uninitialized in @c rooms ,
          absent from @c in_game_rooms , and returned to @c free_room_idxs
*/
void room_leave(AppData* data, Fd fd);

#endif
