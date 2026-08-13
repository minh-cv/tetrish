#ifndef TETRISH_TETRISU_NET_INBOUND_POLICY_H
#define TETRISH_TETRISU_NET_INBOUND_POLICY_H

#include "net/htttp_codec.h"

#include <stdbool.h>

typedef enum {
    NET_INBOUND_STATE_PUSH,
    NET_INBOUND_REPLY,
    NET_INBOUND_LEGACY_ECHO,
    NET_INBOUND_REJECT,
} NetInboundDisposition;

/*!
    @brief classify one authenticated, decoded server message

    `STATE` is a one-way push and is accepted independently of request state.
    A response or byte-for-byte legacy request echo is accepted only while a
    client request is awaiting its terminal result. Every other server request
    is rejected: the client never routes it back into the encoder.

    @pre @p message is a successfully decoded HTTTP message
    @pre @p exact_pending_echo is true only for a byte-for-byte comparison with
         the retained pending plaintext
    @post all arguments are unchanged

    @return the only permitted interpretation of @p message
*/
NetInboundDisposition net_inbound_classify(
    const OwnedHtttpMessage* message,
    bool request_awaiting_reply,
    bool exact_pending_echo
);

#endif
