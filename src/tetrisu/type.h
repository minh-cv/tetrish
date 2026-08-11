#ifndef TETRISH_TETRISU_TYPE_H
#define TETRISH_TETRISU_TYPE_H

/*
    Shared value types and collection instantiations. The macro-template
    collections must be instantiated exactly once per translation unit, so every
    instantiation shared between layers lives here; layer-private ones live in
    the layer's own header.

    FrameStatus, AuthFrame and HtttpOutboundMessage are what src/tetrisd/type.h
    declares, minus the sparse sets: the client has one connection, so every
    `{Fd: [Object]}` collapses to `[Object]`. They are duplicated rather than
    shared for now, and are the obvious candidates to lift into corestack once
    both sides have settled.
*/

#include "htttp.h"          // IWYU pragma: keep
#include "network/reader.h" // IWYU pragma: keep
#include "network/writer.h" // IWYU pragma: keep

typedef enum {
    FRAME_OK,
    FRAME_DECRYPT_ERROR,
    FRAME_PAYLOAD_TOO_LARGE,
    FRAME_HTTTP_PARSE_ERROR,
} FrameStatus;

typedef struct {
    ReaderFrameContent frame;
    FrameStatus status;
} AuthFrame;

#define RING_BUFFER_ELEM_TYPE AuthFrame
#define RING_BUFFER_TYPEDEF AuthFrameQueue
#include "collection/ring_buffer.h"

typedef struct {
    HtttpMessage message;
    HtttpMessageOwnership ownership;
} HtttpOutboundMessage;

#define RING_BUFFER_ELEM_TYPE HtttpOutboundMessage
#define RING_BUFFER_TYPEDEF HtttpOutboundMessageQueue
#include "collection/ring_buffer.h"

/*!
    @brief The single-connection replacement for tetrisd's error set. A layer
    that cannot commit its pass sets a fault; every later layer in the tick
    skips its work while a fault is set, and the tick ends by tearing the
    connection down.
*/
typedef enum {
    FAULT_NONE,
    FAULT_TRANSPORT,  // socket error, EOF, handshake or crypto failure
    FAULT_PROTOCOL,   // the peer sent something the client cannot honour
    FAULT_LOCAL,      // allocation failure, terminal failure
} ClientFault;

/*!
    @brief a human-readable phrase for @p fault

    @return a string literal, never NULL
*/
const char* client_fault_string(ClientFault fault);

#endif
