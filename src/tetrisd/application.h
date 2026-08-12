#ifndef TETRISH_TETRISD_APPLICATION_H
#define TETRISH_TETRISD_APPLICATION_H

#include "auth.h"
#include "player_io.h"
#include "type.h"

#include <stdint.h>

#define SERVER_PLAYER_NAME_MAX 32u

typedef struct {
    char name[SERVER_PLAYER_NAME_MAX + 1u];
} ServerPlayer;

#define SPARSE_SET_ELEM_TYPE ServerPlayer
#define SPARSE_SET_TYPEDEF SparseSet_ServerPlayer
#include "collection/sparse_set.h"

/*!
    @invariant a key is present in @c players iff that authenticated fd has
               application state and has not entered the close accumulator
*/
typedef struct {
    SparseSet_ServerPlayer players;
    uint64_t state_sequence;
} ServerApplication;

/*!
    @brief initialize the per-player reference application

    @pre @p application is not initialized
    @post @c players has capacity @p max_entries and size `0`
    @post the STATE sequence is `0`

    @return `0` on success, `-1` on allocation failure
*/
int ServerApplication_init(ServerApplication* application, size_t max_entries);

/*!
    @brief release the player table
    @pre @p application is initialized and has not been freed
    @post the table allocation is released
*/
void ServerApplication_free(ServerApplication* application);

/*!
    @brief create default application records for newly authenticated fds

    @pre @p application and @p auth are initialized with equal capacities
    @post every authenticated fd not in @p close_fds is present in @c players
    @post existing player metadata is unchanged
*/
void ServerApplication_sync_authenticated(
    ServerApplication* application,
    const AuthData* auth,
    const SparseSet_bool* close_fds
);

/*!
    @brief parse authenticated HTTTP requests and queue typed responses

    Supported methods are `HTTTP`, `SET_PLAYER_NAME`, and `WHOAMI`. Request
    plaintext remains owned by @p decrypted; serialized responses are appended
    to @p response_qs and owned there. Protocol/decryption failures and response
    backpressure add the fd to @p close_fds.

    @pre every active fd in @p decrypted is authenticated and present in players
    @pre @p response_qs has initialized per-fd queues for those fds
    @post each valid request queues exactly one response unless the fd is closed
    @post request buffers and @p decrypted membership are unchanged
*/
void ServerApplication_handle_requests(
    ServerApplication* application,
    const SparseSet_AuthFrameQueue* decrypted,
    SparseSet_AuthFrameQueue* response_qs,
    SparseSet_bool* close_fds
);

/*!
    @brief produce one latest STATE snapshot for every output-idle player

    @pre all arguments are initialized and use the same fd-table capacity
    @pre @p expirations is nonzero
    @post the sequence advances by @p expirations
    @post a player with a response, queued output, partial output, or close mark
          receives no STATE this pass
    @post every other authenticated player gets at most one STATE request marked
          as best-effort push in @p response_qs
*/
void ServerApplication_push_state(
    ServerApplication* application,
    const AuthData* auth,
    const PlayerIo* io,
    uint64_t expirations,
    SparseSet_AuthFrameQueue* response_qs,
    const SparseSet_bool* close_fds
);

/*!
    @brief remove application records for closing fds
    @pre every key in @p close_fds is within the player-table capacity
    @post no key in @p close_fds is present in @c players
*/
void ServerApplication_close(
    ServerApplication* application,
    const SparseSet_bool* close_fds
);

#endif
