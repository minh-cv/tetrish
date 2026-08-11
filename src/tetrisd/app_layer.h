#ifndef TETRISH_TETRISD_APP_LAYER_H
#define TETRISH_TETRISD_APP_LAYER_H

#include "app/effect.h"
#include "app/world.h"
#include "htttp_layer.h"
#include "type.h"

/*!
    @brief Per-fd persistent state: the handle into the world, and nothing
    else. Everything the rules care about lives in the world, keyed by handle
    rather than by fd, which is what lets a room outlive the connection that
    created it.
*/
typedef struct {
    PlayerRef self;
} AppEntry;

#define SPARSE_SET_ELEM_TYPE AppEntry
#define SPARSE_SET_TYPEDEF SparseSet_AppEntry
#include "collection/sparse_set.h"

/*!
    The top layer of the pipeline. It owns no queues: the parsed input
    ( @c parsed_qs ) and the response output ( @c response_qs ) both belong to
    the htttp layer, as each layer owns the queue pair at its boundary with
    the layer above.

    The layer is split in two passes on purpose. @c AppData_respond decides
    what should happen and records it in @c sink as handle-addressed effects;
    @c AppData_flush is the only place that turns a handle into an fd and the
    only place that allocates an outbound message. Fan-out — one request
    producing output on connections that sent nothing this tick — needs
    exactly that separation.

    @invariant A key @c fd is in @c entries iff the fd has been accepted and
    not closed, and its @c self handle resolves in @c world .
*/
typedef struct {
    SparseSet_AppEntry entries;
    World world;
    AppEffectSink sink;
} AppData;

/*!
    @brief allocate memory to members of @p data

    @pre @p data is not initialized

    @post @c entries and the world have capacity @p max_entries , and @c sink
          can hold @p effect_capacity effects with @p arena_capacity bytes of
          body between them.

    @return -1 if failed, 0 otherwise
*/
int AppData_init(AppData* data, size_t max_entries, size_t effect_capacity, size_t arena_capacity);

/*!
    @brief release all memory in @p data

    @pre @p data has not been freed
*/
void AppData_free(AppData* data);

/*!
    @brief discard the effects collected this tick

    @pre every effect has been flushed
    @post @c sink is empty; nothing else changes
*/
void AppData_reset(AppData* data);

/*!
    @brief initialize entries for each entry in @p fds

    @pre entries in @p fds exist neither in @c entries nor @p err_fds

    @post entries in @p fds not marked in @p err_fds are in @c entries and
          have a live player in @c world , named `player<fd>` by default.

    @note if an entry in @p fds appear in @c entries or @c err_fds , that entry is ignored. This is not part of the contract.
*/
void AppData_accept(
    AppData* data,
    const Vec_Fd* fds,
    SparseSet_bool* err_fds
);

/*!
    @brief remove entries in @c entries for each entry in @p close_fds

    @pre the entries in @p close_fds must exist in @c entries

    @post the slot in @p close_fds is uninitialized in @c entries and its
          player is released from @c world , so handles other parts of the
          world still hold for it stop resolving.

    @note if an entry does not exist in @c entries , it is ignored. This is not part of the contract.
*/
void AppData_close(
    AppData* data,
    const SparseSet_bool* close_fds
);

/*!
    @brief Route every parsed message of every fd in @p m_parsed_qs through
           the world, collecting what should be sent into @c sink .

    A message that could not be parsed, or that does not name a method this
    daemon routes, produces one error reply and leaves the connection open.
    The world's own rejections are replies too. Only failing to record an
    effect — the sink is full — fails the fd.

    @pre  No entry of @p m_parsed_qs is already marked in @p err_fds

    @post For each fd whose effects could not be recorded, it is newly marked
          in @p err_fds . Pre-existing entries of @p err_fds are preserved.
    @post @c sink holds the effects of every fd not marked in @p err_fds .

    @note If the precondition is violated, the overlapping entries are
          currently skipped. This is not part of the contract.
*/
void AppData_respond(
    AppData* data,
    const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
    SparseSet_bool* err_fds
);

/*!
    @brief Turn every effect in @c sink into an outbound message in its
           target's slot of @p m_response_qs .

    This is the only place a handle becomes an fd. An effect whose target no
    longer resolves, or whose fd is already marked in @p err_fds , is dropped
    silently: the connection it was for is gone or going.

    @pre  @p m_response_qs slots were accepted with a capacity of at least the
          number of effects any one target can receive in a tick.

    @post For each fd whose message could not be built, it is newly marked in
          @p err_fds with its slot in @p m_response_qs inactive, along with
          messages there freed.
    @post Entry in @p m_response_qs is active iff its queue has size of at
          least 1. Messages appended there are owned by @p m_response_qs and
          reclaimed by HtttpData_reset.
*/
void AppData_flush(
    AppData* data,
    SparseSet_HtttpOutboundMessageQueue* m_response_qs,
    SparseSet_bool* err_fds
);

#endif
