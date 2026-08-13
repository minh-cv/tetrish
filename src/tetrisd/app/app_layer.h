#ifndef TETRISH_TETRISD_APP_LAYER_H
#define TETRISH_TETRISD_APP_LAYER_H

#include "htttp_layer.h"
#include "tetrisbrain/state.h"
#include "type.h"
#include <stdint.h>

#define PLAYER_NAME_MAX 20

//! @brief @c Player.room_idx sentinel for not in a room
#define ROOM_IDX_NONE SIZE_MAX

typedef enum {
    //! @brief room exists, no game running; @c Room.game is meaningless
    ROOM_LOBBY,
    //! @brief @c Room.game is a live board
    ROOM_IN_GAME,
} RoomStatus;

/*!
    @brief Per-fd persistent state. The player id is the key itself (the
    fd), so it is not stored.
*/
typedef struct {
    //! @brief null-terminated, empty string as sentinel for no name set
    char name[PLAYER_NAME_MAX + 1];

    //! @brief key into @c AppData.rooms , @c ROOM_IDX_NONE when in no room
    size_t room_idx;
} Player;

/*!
    @brief One player and the game they are in.

    A room holds exactly one member for now, so the member and the board
    are plain fields. Battle royale replaces both with a per-member
    collection; nothing here is meant to generalize by tweaking a bound.

    @invariant @c member is a key in @c AppData.players whose @c room_idx
               is this room's key.
    @invariant @c game is only meaningful while `status == ROOM_IN_GAME` .
*/
typedef struct {
    Fd member;
    RoomStatus status;
    State game;
} Room;

#define SPARSE_SET_ELEM_TYPE Player
#define SPARSE_SET_TYPEDEF SparseSet_Player
#include "collection/sparse_set.h"

#define SPARSE_SET_ELEM_TYPE Room
#define SPARSE_SET_TYPEDEF SparseSet_Room
#include "collection/sparse_set.h"

/*
    Room keys are not derived from anything, unlike player keys, which are
    fds. Free ones are handed out from this stack rather than scanned for,
    since the sparse set has no free-key search.
*/
#define RING_BUFFER_ELEM_TYPE size_t
#define RING_BUFFER_TYPEDEF Vec_RoomIdx
#include "collection/ring_buffer.h"

/*!
    The top layer of the pipeline: it owns no queues of its own. Each layer
    owns the queue pair at its boundary with the layer above, so both the
    parsed input ( @c parsed_qs ) and the response output ( @c response_qs )
    it works on belong to the htttp layer.

    @invariant A key @c fd is in @c players iff the fd has been accepted and
    not closed.

    @invariant A key is in @c rooms iff it is not in @c free_room_idxs .

    @invariant @c in_game_rooms holds exactly the keys of @c rooms whose
    status is @c ROOM_IN_GAME .
*/
typedef struct {
    SparseSet_Player players;
    SparseSet_Room rooms;

    //! @brief iteration index for the tick
    SparseSet_bool in_game_rooms;

    Vec_RoomIdx free_room_idxs;
} AppData;

/*!
    @brief allocate memory to members of @p data

    @pre @p data is not initialized

    @post @c players has capacity @p max_entries and size `0`, with all
          elements uninitialized.
    @post @c rooms and @c in_game_rooms have capacity @p max_rooms and
          size `0`, with all elements uninitialized, and every key of
          @c rooms is in @c free_room_idxs .

    @return -1 if failed, 0 otherwise
*/
int AppData_init(AppData* data, size_t max_entries, size_t max_rooms);

/*!
    @brief release all memory in @p data

    @pre @p data has not been freed

    @post All initialized entries in @c players and @c rooms are
          uninitialized
    @post All collection members are freed
*/
void AppData_free(AppData* data);

/*!
    @brief initialize entries for each entry in @p fds

    @pre entries in @p fds exist neither in @c players nor @p err_fds

    @post entries in @p fds not marked in @p err_fds are in @c players ,
          with no name and @c room_idx of @c ROOM_IDX_NONE .
          Inserting an entry cannot fail within the capacity contract, so
          no fd is ever marked in @p err_fds here.

    @note if an entry in @p fds appear in @c players or @c err_fds , that entry is ignored. This is not part of the contract.
*/
void AppData_accept(
    AppData* data,
    const Vec_Fd* fds,
    SparseSet_bool* err_fds
);

/*!
    @brief remove entries in @c players for each entry in @p close_fds

    @pre the entries in @p close_fds must exist in @c players

    @post for each entry that was in a room, that room's key is
          uninitialized in @c rooms , absent from @c in_game_rooms , and
          back in @c free_room_idxs
    @post the slot in @p close_fds is uninitialized in @c players

    @note if an entry does not exist in @c players , it is ignored. This is not part of the contract.
*/
void AppData_close(
    AppData* data,
    const SparseSet_bool* close_fds
);

/*!
    @brief Dummy application layer: for every fd in @p m_parsed_qs , build
           one default response per parsed entry into its slot in
           @p m_response_qs , marking fds whose response could not be
           built in @p err_fds .

    A valid request gets a 200 echoing its body. Anything else — a parse
    error (including in-band transport errors: decrypt failure, oversized
    read) or a response-typed message — gets a bodyless 400; the
    connection stays open either way.

    @pre  No entry of @p m_parsed_qs is already marked in @p err_fds
    @pre  @p m_parsed_qs and @p m_response_qs slots were accepted with the
          same queue capacity (see server_tick's accept fan-out), so the
          output always fits.

    @post For each failed fd in @p m_parsed_qs , it is newly marked in
          @p err_fds with its slot in @p m_response_qs inactive, along
          with messages there freed. Pre-existing entries of @p err_fds
          are preserved.
    @post Entry in @p m_response_qs is active iff its queue has size of at least 1. Messages appended there are owned by @p m_response_qs and reclaimed by HtttpData_reset.

    @note If the first precondition is violated, the overlapping entries
          are currently skipped. If the second is violated, the fd is
          currently failed like an operation failure. Neither behavior is
          part of the contract and must not be relied upon.

    TODO: replace the echo with real game logic (libtetrisbrain).
*/
void AppData_respond(
    AppData* data,
    const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
    SparseSet_HtttpOutboundMessageQueue* m_response_qs,
    SparseSet_bool* err_fds
);

#endif
