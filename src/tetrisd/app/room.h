#ifndef TETRISH_TETRISD_APP_ROOM_H
#define TETRISH_TETRISD_APP_ROOM_H

#include "app/app_layer.h"
#include "type.h"
#include <stddef.h>

/*!
    @brief put @p fd in a room of its own, in @c ROOM_LOBBY

    @pre @p fd is a key in @c players

    @post on success, a fresh key of @c rooms holds a room of one member
          with no game, @p fd 's @c room_idx is that key, and the key is
          no longer in @c free_room_idxs . @p out_room_idx receives it.
    @post on failure, nothing changed

    @return -1 if @p fd is already in a room or no room key is free, 0
            otherwise
*/
int room_create(AppData* data, Fd fd, size_t* out_room_idx);

/*!
    @brief start the game of the room keyed @p room_idx

    @pre @p room_idx is a key in @c rooms

    @post the room is @c ROOM_IN_GAME with a freshly initialized board,
          and @p room_idx is in @c in_game_rooms

    @note starting an already started room restarts its board. This is not
          part of the contract.
*/
void room_start(AppData* data, size_t room_idx);

/*!
    @brief take @p fd out of its room, if it is in one

    @pre @p fd is a key in @c players

    @post @p fd 's @c room_idx is @c ROOM_IDX_NONE
    @post if @p fd was in a room, that room's key is uninitialized in
          @c rooms , absent from @c in_game_rooms , and returned to
          @c free_room_idxs
*/
void room_leave(AppData* data, Fd fd);

#endif
