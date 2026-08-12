#ifndef TETRISH_TETRISD_HTTTP_LAYER_H
#define TETRISH_TETRISD_HTTTP_LAYER_H

#include "type.h"

/*!
    @brief A decrypted frame parsed in place. On FRAME_OK,
    @c message 's pointers are non-owning views into the decrypt_qs frame it
    was parsed from, valid until AuthData_reset reclaims that frame. There
    is no ownership mask: htttp_parse never allocates. On any other status
    (transport-level error status on the input frame, or malformed HTTTP),
    @c message is zeroed.
*/
typedef struct {
    HtttpMessage message;
    FrameStatus status;
} HtttpParsedMessage;

#define RING_BUFFER_ELEM_TYPE HtttpParsedMessage
#define RING_BUFFER_TYPEDEF HtttpParsedMessageQueue
#include "collection/ring_buffer.h"

#define SPARSE_SET_ELEM_TYPE HtttpParsedMessageQueue
#define SPARSE_SET_TYPEDEF SparseSet_HtttpParsedMessageQueue
#include "collection/sparse_set.h"

/*!
    @brief Per-fd persistent state. Currently a membership marker only,
    since parsing is per-frame; real per-fd HTTTP state (e.g. keep-alive
    bookkeeping) would go here.
*/
typedef struct {
    char _reserved;
} HtttpEntry;

#define SPARSE_SET_ELEM_TYPE HtttpEntry
#define SPARSE_SET_TYPEDEF SparseSet_HtttpEntry
#include "collection/sparse_set.h"

/*!
    @invariant A key @c fd is in @c entries iff the associated @c parsed_qs and @c response_qs (collectively called @c *_qs ) slots are initialized.

    @invariant Every entry in @c *_qs is active iff the underlying queue has size of at least 1.
*/
typedef struct {
    SparseSet_HtttpEntry entries;
    SparseSet_HtttpParsedMessageQueue parsed_qs;
    SparseSet_HtttpOutboundMessageQueue response_qs;
} HtttpData;

/*!
    @brief allocate memory to members of @p data

    @pre @p data is not initialized

    @post All collection members have capacity @p max_entries and size `0`.
    @post @c *_qs has all elements uninitialized.
    @post All elements of @c entries are uninitialized.

    @return -1 if failed, 0 otherwise
*/
int HtttpData_init(HtttpData* data, size_t max_entries);

/*!
    @brief release all memory in @p data

    @pre @p data has not been freed

    @post All messages in @c response_qs are freed
    @post All initialized entries in @c *_qs and @c entries are uninitialized
    @post All collection members are freed
*/
void HtttpData_free(HtttpData* data);

/*!
    @brief reset the per-loop state for next iteration

    @post All entries in @c *_qs is inactive, with all elements in each entry reclaimed ( @c response_qs messages freed; @c parsed_qs holds only non-owning views, so nothing is freed there).
    @post other members do not change
*/
void HtttpData_reset(HtttpData* data);

/*!
    @brief initialize entries for each entry in @p fds , appending failed entries in @p err_fds

    @pre entries in @p fds exist neither in @c entries nor @p err_fds

    @post successful entries in @p fds has their slot in @c *_qs initialized and in inactive state. Those slot has queue capacity @p queue_capacity and size 0.
    @post successful entries in @p fds is in @c entries
    @post failed entries in @p fds are appended to @p err_fds

    @note if an entry in @p fds appear in @c entries or @c err_fds , that entry is ignored. This is not part of the contract.
*/
void HtttpData_accept(
    HtttpData* data,
    const Vec_Fd* fds,
    SparseSet_bool* err_fds,
    size_t queue_capacity
);

/*!
    @brief remove entries in @c entries for each entry in @p close_fds

    @pre the entries in @p close_fds must exist in @c entries

    @post the slot in @p close_fds is uninitialized in @c entries . If exists, all elements in their slot of @c *_qs are reclaimed. Those slots are also uninitialized.

    @note if an entry does not exist in @c entries , it is ignored. This is not part of the contract.
*/
void HtttpData_close(
    HtttpData* data,
    const SparseSet_bool* close_fds
);

/*!
    @brief Parse every decrypted frame of every fd in @p m_decrypt_qs in
           place, appending the results to its slot in @p m_parsed_qs .

    A frame is parsed only if both its AuthFrameStatus and its embedded
    ReaderFrameStatus are OK; otherwise HTTTP_LAYER_PARSE_ERROR travels
    in-band in @p m_parsed_qs . Within the contract, parsing has no
    operation-failure path, so no fd is ever marked in @p err_fds here.

    @pre  No entry of @p m_decrypt_qs is already marked in @p err_fds
    @pre  @p m_decrypt_qs and @p m_parsed_qs slots were accepted with the
          same queue capacity (both receive cfg.client_capacity, see
          server_tick's accept fan-out), so the output always fits.

    @post Frame contents in @p m_decrypt_qs may be modified in place
          (parsing splits them into strings); their ownership does not
          change.
    @post Entry in @p m_parsed_qs is active iff its queue has size of at least 1.

    @note If the first precondition is violated, the overlapping entries
          are currently skipped. If the second is violated, the fd is
          currently failed (marked in @p err_fds with its slot in
          @p m_parsed_qs inactive). Neither behavior is part of the
          contract and must not be relied upon.
*/
void HtttpData_parse(
    HtttpData* data,
    const SparseSet_AuthFrameQueue* m_decrypt_qs,
    SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
    SparseSet_bool* err_fds
);

/*!
    @brief Serialize every outbound message of every fd in @p m_response_qs ,
           appending the serialized buffers as AuthFrames to its slot in
           @p m_auth_qs and marking fds that should be closed in @p err_fds .

    A serialization failure (allocation failure, or a serialized message
    that is empty or exceeds PLAINTEXT_FRAME_MAX) is an operation failure
    for that fd, not an in-band condition. The bound is on the plaintext
    that still fits a frame once encrypted, since AuthData_encrypt consumes
    these buffers next.

    @pre  No entry of @p m_response_qs is already marked in @p err_fds
    @pre  @p m_response_qs and @p m_auth_qs slots were accepted with the
          same queue capacity (see server_tick's accept fan-out), so the
          output always fits.

    @post For each failed fd in @p m_response_qs , it is newly marked in
          @p err_fds with its slot in @p m_auth_qs inactive, along with
          frames there freed. Pre-existing entries of @p err_fds are
          preserved.
    @post Entry in @p m_auth_qs is active iff its queue has size of at least 1. Frames appended there are owned by @p m_auth_qs and reclaimed by AuthData_reset.

    @note If the first precondition is violated, the overlapping entries
          are currently skipped. If the second is violated, the fd is
          currently failed like an operation failure. Neither behavior is
          part of the contract and must not be relied upon.
*/
void HtttpData_serialize(
    HtttpData* data,
    const SparseSet_HtttpOutboundMessageQueue* m_response_qs,
    SparseSet_WriterFrameQueue* m_encrypt_qs,
    SparseSet_bool* err_fds
);

#endif
