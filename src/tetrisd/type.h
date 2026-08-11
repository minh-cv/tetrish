#ifndef TETRISH_TETRISD_TYPE_H
#define TETRISH_TETRISD_TYPE_H

/*
    Shared value types and collection instantiations. The macro-template
    collections must be instantiated exactly once per translation unit, so every
    instantiation shared between layers lives here; layer-private ones live in the
    layer's own header.
*/

#include "htttp.h" // IWYU pragma: keep
#include "network/reader.h" // IWYU pragma: keep
#include "network/writer.h" // IWYU pragma: keep
#include <stdint.h>

typedef int Fd;
typedef uint32_t EpollInterest;

#define RING_BUFFER_ELEM_TYPE Fd
#define RING_BUFFER_TYPEDEF Vec_Fd
#include "collection/ring_buffer.h"

#define SPARSE_SET_ELEM_TYPE bool
#define SPARSE_SET_TYPEDEF SparseSet_bool
#include "collection/sparse_set.h"

#define SPARSE_SET_ELEM_TYPE WriterFrameQueue
#define SPARSE_SET_TYPEDEF SparseSet_WriterFrameQueue
#include "collection/sparse_set.h"

#define SPARSE_SET_ELEM_TYPE ReaderFrameQueue
#define SPARSE_SET_TYPEDEF SparseSet_ReaderFrameQueue
#include "collection/sparse_set.h"

typedef enum {
    WRITER_QUEUE_EMPTY,
    WRITER_QUEUE_NORMAL,
    WRITER_QUEUE_FULL,
} WriterQueueStatus;

typedef struct {
    Fd fd;
    WriterQueueStatus status;
} WriterQueueStatusEntry;

#define RING_BUFFER_ELEM_TYPE WriterQueueStatusEntry
#define RING_BUFFER_TYPEDEF Vec_WriterQueueStatusEntry
#include "collection/ring_buffer.h"

typedef enum {
    AUTH_FRAME_OK,
    AUTH_FRAME_DECRYPT_FAILURE,
} AuthFrameStatus;

typedef struct {
    ReaderFrame frame;
    AuthFrameStatus status;
} AuthFrame;

#define RING_BUFFER_ELEM_TYPE AuthFrame
#define RING_BUFFER_TYPEDEF AuthFrameQueue
#include "collection/ring_buffer.h"

#define SPARSE_SET_ELEM_TYPE AuthFrameQueue
#define SPARSE_SET_TYPEDEF SparseSet_AuthFrameQueue
#include "collection/sparse_set.h"

/*
    An outbound HTTTP message staged for serialization, with the ownership
    mask htttp_message_free needs to reclaim it. Lives here rather than in
    the htttp layer's header because the application layer produces this
    state, so it crosses layers.
*/
typedef struct {
    HtttpMessage message;
    HtttpMessageOwnership ownership;
} HtttpOutboundMessage;

#define RING_BUFFER_ELEM_TYPE HtttpOutboundMessage
#define RING_BUFFER_TYPEDEF HtttpOutboundMessageQueue
#include "collection/ring_buffer.h"

#define SPARSE_SET_ELEM_TYPE HtttpOutboundMessageQueue
#define SPARSE_SET_TYPEDEF SparseSet_HtttpOutboundMessageQueue
#include "collection/sparse_set.h"

#endif
