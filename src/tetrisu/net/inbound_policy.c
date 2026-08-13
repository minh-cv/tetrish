#include "net/inbound_policy.h"

NetInboundDisposition net_inbound_classify(
    const OwnedHtttpMessage* message,
    bool request_awaiting_reply,
    bool exact_pending_echo
) {
    if (htttp_codec_is_state_push(message)) {
        return NET_INBOUND_STATE_PUSH;
    }
    if (request_awaiting_reply && !message->view.is_request) {
        return NET_INBOUND_REPLY;
    }
    if (request_awaiting_reply && exact_pending_echo) {
        return NET_INBOUND_LEGACY_ECHO;
    }
    return NET_INBOUND_REJECT;
}
