#ifndef TETRISH_TETRISU_NET_INBOUND_POLICY_H
#define TETRISH_TETRISU_NET_INBOUND_POLICY_H

#include "net/htttp_codec.h"

#include <stdbool.h>

typedef enum {
    NET_INBOUND_STATE_PUSH,
    NET_INBOUND_REPLY,
    NET_INBOUND_LEGACY_ECHO,
    //! @brief a response nothing asked for: reportable, not fatal
    NET_INBOUND_UNSOLICITED_REPLY,
    NET_INBOUND_REJECT,
} NetInboundDisposition;

/*!
    @brief classify one authenticated, decoded server message

    `STATE` is a one-way push and is accepted independently of request state.
    A response or byte-for-byte legacy request echo is accepted only while a
    client request is awaiting its terminal result.

    A response arriving with nothing outstanding is neither of those and is
    still not a protocol violation. `tetrisd` answers frame-level failures —
    an undecryptable, oversized or unparseable frame — before it knows which
    method the frame carried, and gameplay inputs complete on send, so a
    corrupted frame during play is answered while the client awaits nothing.
    Tearing the session down for that would cost a match in progress over a
    single bad frame, so it is surfaced and discarded instead.

    Every other server request is rejected: an unknown one is a genuine
    protocol violation, and the client never routes it back into the encoder.

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
