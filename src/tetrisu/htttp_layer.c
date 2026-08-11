#include "htttp_layer.h"
#include "wire.h"
#include <stdlib.h>
#include <string.h>

int HtttpData_init(HtttpData* data, size_t queue_capacity) {
    return HtttpParsedMessageQueue_init(&data->parsed_q, queue_capacity);
}

void HtttpData_free(HtttpData* data) {
    HtttpParsedMessageQueue_free(&data->parsed_q);
}

void HtttpData_reset(HtttpData* data) {
    HtttpParsedMessageQueue_reset(&data->parsed_q);
}

void HtttpData_parse(HtttpData* data, const AuthFrameQueue* m_decrypt_q,
                     HtttpParsedMessageQueue* m_parsed_q, ClientFault* fault) {
    (void)data;
    if (*fault != FAULT_NONE) {
        return;
    }

    const size_t count = AuthFrameQueue_size(m_decrypt_q);
    for (size_t i = 0; i < count; i++) {
        const AuthFrame* const frame = AuthFrameQueue_at(m_decrypt_q, i);

        HtttpParsedMessage parsed;
        memset(&parsed, 0, sizeof(parsed));
        parsed.status = frame->status;

        if (frame->status == FRAME_OK &&
            htttp_parse(frame->frame.ptr, frame->frame.length, &parsed.message) == -1) {
            memset(&parsed.message, 0, sizeof(parsed.message));
            parsed.status = FRAME_HTTTP_PARSE_ERROR;
        }

        if (HtttpParsedMessageQueue_push_back(m_parsed_q, &parsed) == -1) {
            // the queues share a capacity, so this is a broken precondition
            *fault = FAULT_LOCAL;
            return;
        }
    }
}

void HtttpData_serialize(HtttpData* data, HtttpOutboundMessageQueue* m_request_q,
                         WriterFrameQueue* m_encrypt_q, ClientFault* fault) {
    (void)data;

    const size_t count = HtttpOutboundMessageQueue_size(m_request_q);
    for (size_t i = 0; i < count; i++) {
        HtttpOutboundMessage* const message = HtttpOutboundMessageQueue_front(m_request_q);

        if (*fault == FAULT_NONE) {
            size_t length;
            unsigned char* const buffer = htttp_serialize(&message->message, &length);
            if (buffer == NULL || length == 0 || length > FRAME_MAX) {
                free(buffer);
                *fault = FAULT_LOCAL;
            }
            else {
                const WriterFrame frame = {buffer, length};
                if (WriterFrameQueue_push_back(m_encrypt_q, &frame) == -1) {
                    free(buffer);
                    *fault = FAULT_LOCAL;
                }
            }
        }

        htttp_message_free(&message->message, &message->ownership);
        HtttpOutboundMessageQueue_pop_front(m_request_q);
    }
}
