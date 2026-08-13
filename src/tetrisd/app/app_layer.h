#ifndef TETRISH_TETRISD_APP_LAYER_H
#define TETRISH_TETRISD_APP_LAYER_H

#include "htttp_layer.h"
#include "tetrisbrain/input.h"
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

    /*!
        @brief keys the member pressed since the last tick

        Per-member state like @c game , and only meaningful alongside it.
        The tick applies the whole set as one frame and clears it, so an
        input request only records a key rather than advancing the game
        itself.
    */
    bool inputs[PLAYER_INPUT_KEY_COUNT];
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
    @brief For every fd in @p m_parsed_qs , act on each parsed entry and
           append the response it produces, if any, to its slot in
           @p m_response_qs , marking fds whose response could not be built
           in @p err_fds .

    A valid request is routed by @c respond_one_request (see
    @c app/dispatch.h ), which decides both the response and whether there
    is one at all. A parse error, including the in-band transport errors
    (decrypt failure, oversized read), gets a response naming the failure.
    A response-typed message is ignored. The connection stays open in
    every case.

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
*/
void AppData_respond(
    AppData* data,
    const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
    SparseSet_HtttpOutboundMessageQueue* m_response_qs,
    SparseSet_bool* err_fds
);

/*!
    @brief Advance every running game by @p expirations frames and push each
           member the snapshot the last of them produced, marking fds whose
           snapshot could not be built in @p err_fds .

    A tick is the only thing that moves a board. An input request records a
    key (see @c app/dispatch.h ) and this applies the whole recorded set as
    one frame, then clears it, so a player pressing the same key twice
    between two ticks moves once. The snapshot travels as a
    server-originated @c STATE request rather than a response, since it
    answers no request of the player's.

    @p expirations is the clock's count of periods elapsed (see
    @c RoomTimer_read ), so a loop that overran several of them catches the
    games up rather than letting them run slow. Only the recorded inputs go
    into the first of those frames; the rest advance on no input, since
    which frame a key belonged to was not recorded. One snapshot goes out
    however many frames it took, so a member never sees the catch-up as more
    messages than a tick on time.

    A game that tops out on one of its frames ends there: its room drops
    back to @c ROOM_LOBBY , later frames of the same call do not run, and
    the snapshot is the one carrying @c is_game_active false, so the end of
    the game reaches the player as part of the frame that caused it rather
    than as a message of its own.

    @pre  No room of @c in_game_rooms has a member outside @c players
    @pre  Every fd of @c players has a slot in @p m_response_qs (both are
          initialized at accept, see server_tick's accept fan-out)

    @post Nothing happened at all when @p expirations is `0` : no frame runs
          and no snapshot is pushed.
    @post Every room of @c in_game_rooms whose member is not in @p err_fds
          has advanced @p expirations frames, or as many as it took to top
          out, and has no pending inputs. A room that topped out is
          @c ROOM_LOBBY and absent from @c in_game_rooms .
    @post For each failed fd, it is newly marked in @p err_fds with its slot
          in @p m_response_qs inactive, along with messages there freed.
          Pre-existing entries of @p err_fds are preserved.
    @post Entry in @p m_response_qs is active iff its queue has size of at
          least 1. Messages appended there are owned by @p m_response_qs and
          reclaimed by HtttpData_reset.

    @note A member already in @p err_fds is closing this tick, so its room
          is left alone for AppData_close to reclaim: the game neither
          advances nor reports.
    @note Unlike the layers below, this one emits output no input asked for,
          so a response queue with no room left is a reachable state rather
          than a broken capacity contract, and is not treated as a failure.
          A snapshot that does not fit is dropped and the next tick sends a
          fresh one. A game-over snapshot has no next tick, so give
          @p m_response_qs one slot more than the read queue if that loss
          matters. Neither the drop nor its bound is part of the contract.
*/
void AppData_room_tick(
    AppData* data,
    uint64_t expirations,
    SparseSet_HtttpOutboundMessageQueue* m_response_qs,
    SparseSet_bool* err_fds
);

#endif
