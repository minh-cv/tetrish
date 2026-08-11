#ifndef TETRISH_TETRISU_HTTTP_LAYER_H
#define TETRISH_TETRISU_HTTTP_LAYER_H

#include "type.h"

/*!
    @brief A decrypted frame parsed in place. On FRAME_OK, @c message 's
    pointers are non-owning views into the decrypt_q frame it was parsed from,
    valid until AuthData_reset reclaims that frame. On any other status
    @c message is zeroed.
*/
typedef struct {
    HtttpMessage message;
    FrameStatus status;
} HtttpParsedMessage;

#define RING_BUFFER_ELEM_TYPE HtttpParsedMessage
#define RING_BUFFER_TYPEDEF HtttpParsedMessageQueue
#include "collection/ring_buffer.h"

/*!
    @brief The mirror image of tetrisd's HTTTP layer: it serializes requests
    and parses responses. It must also accept inbound *requests*, since the
    server pushes STATE that way.
*/
typedef struct {
    HtttpParsedMessageQueue parsed_q;
} HtttpData;

int HtttpData_init(HtttpData* data, size_t queue_capacity);
void HtttpData_free(HtttpData* data);

/*!
    @post @c parsed_q is empty. Nothing is freed: the messages are views.
*/
void HtttpData_reset(HtttpData* data);

/*!
    @brief parse every frame of @p m_decrypt_q in place into @p m_parsed_q

    @pre  @p m_decrypt_q and @p m_parsed_q have the same capacity, so the
          one-message-per-frame output always fits
    @post a malformed message travels in-band as FRAME_HTTTP_PARSE_ERROR
    @post frame contents may be modified in place; ownership does not change
*/
void HtttpData_parse(HtttpData* data, const AuthFrameQueue* m_decrypt_q,
                     HtttpParsedMessageQueue* m_parsed_q, ClientFault* fault);

/*!
    @brief serialize every message of @p m_request_q into @p m_encrypt_q

    @post @p m_request_q is drained and its messages freed
    @post a serialization failure, an empty result, or a result over FRAME_MAX
          sets @p fault to FAULT_LOCAL
*/
void HtttpData_serialize(HtttpData* data, HtttpOutboundMessageQueue* m_request_q,
                         WriterFrameQueue* m_encrypt_q, ClientFault* fault);

#endif
