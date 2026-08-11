#include "htttp_layer.h"
#include "dtor.h"
#include "htttp.h"
#include "wire.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(SparseSet_HtttpEntry_free)
static DTOR_WRAPPER_DEFINE(SparseSet_HtttpParsedMessageQueue_free)

/*!
    @see auth_queue_drain (auth.c) — intentional duplicate, used for the
    serialize rollback path
*/
static void auth_queue_drain(AuthFrameQueue* q) {
    const size_t count = AuthFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const AuthFrame* frame = AuthFrameQueue_front(q);
        if (frame->status == AUTH_FRAME_OK && frame->frame.status == READER_FRAME_OK) {
            free(frame->frame.content.ptr);
        }
        AuthFrameQueue_pop_front(q);
    }
}

static void response_queue_drain(HtttpOutboundMessageQueue* q) {
    const size_t count = HtttpOutboundMessageQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        HtttpOutboundMessage* m = HtttpOutboundMessageQueue_front(q);
        htttp_message_free(&m->message, &m->ownership);
        HtttpOutboundMessageQueue_pop_front(q);
    }
}

int HtttpData_init(HtttpData* data, size_t max_entries) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (SparseSet_HtttpEntry_init(&data->entries, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_HtttpEntry_free, &data->entries);

    if (SparseSet_HtttpParsedMessageQueue_init(&data->parsed_qs, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_HtttpParsedMessageQueue_free, &data->parsed_qs);

    if (SparseSet_HtttpOutboundMessageQueue_init(&data->response_qs, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

void HtttpData_free(HtttpData* data) {
    for (size_t i = 0; i < SparseSet_HtttpEntry_size(&data->entries); i++) {
        const size_t fd = SparseSet_HtttpEntry_key_at_idx(&data->entries, i);

        HtttpParsedMessageQueue* pq = SparseSet_HtttpParsedMessageQueue_value_at(&data->parsed_qs, fd);
        HtttpParsedMessageQueue_free(pq);

        HtttpOutboundMessageQueue* rq = SparseSet_HtttpOutboundMessageQueue_value_at(&data->response_qs, fd);
        response_queue_drain(rq);
        HtttpOutboundMessageQueue_free(rq);
    }
    SparseSet_HtttpOutboundMessageQueue_free(&data->response_qs);
    SparseSet_HtttpParsedMessageQueue_free(&data->parsed_qs);
    SparseSet_HtttpEntry_free(&data->entries);
}

void HtttpData_reset(HtttpData* data) {
    // parsed messages are non-owning views into decrypt_qs frames, so
    // reclaiming them is just emptying the rings
    for (size_t i = 0; i < SparseSet_HtttpParsedMessageQueue_size(&data->parsed_qs); i++) {
        HtttpParsedMessageQueue_reset(SparseSet_HtttpParsedMessageQueue_at_idx(&data->parsed_qs, i));
    }
    SparseSet_HtttpParsedMessageQueue_reset(&data->parsed_qs);

    for (size_t i = 0; i < SparseSet_HtttpOutboundMessageQueue_size(&data->response_qs); i++) {
        response_queue_drain(SparseSet_HtttpOutboundMessageQueue_at_idx(&data->response_qs, i));
    }
    SparseSet_HtttpOutboundMessageQueue_reset(&data->response_qs);
}

void HtttpData_accept(HtttpData* data, const Vec_Fd* fds, SparseSet_bool* err_fds, size_t queue_capacity) {
    for (size_t i = 0; i < Vec_Fd_size(fds); i++) {
        const Fd fd_raw = *Vec_Fd_at(fds, i);
        assert(fd_raw >= 0 && (size_t)fd_raw < data->entries.capacity);
        const size_t fd = (size_t)fd_raw;

        if (SparseSet_HtttpEntry_contains(&data->entries, fd)) {
            assert(false && "accepted fd must not already be in entries");
            continue;
        }
        if (SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }

        HtttpParsedMessageQueue* pq = SparseSet_HtttpParsedMessageQueue_value_at(&data->parsed_qs, fd);
        if (HtttpParsedMessageQueue_init(pq, queue_capacity) == -1) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            continue;
        }
        HtttpOutboundMessageQueue* rq = SparseSet_HtttpOutboundMessageQueue_value_at(&data->response_qs, fd);
        if (HtttpOutboundMessageQueue_init(rq, queue_capacity) == -1) {
            HtttpParsedMessageQueue_free(pq);
            *SparseSet_bool_activate(err_fds, fd) = true;
            continue;
        }

        HtttpEntry entry;
        memset(&entry, 0, sizeof(entry));
        const int err = SparseSet_HtttpEntry_insert(&data->entries, fd, &entry);
        assert(err != -1);
        (void)err;
    }
}

void HtttpData_close(HtttpData* data, const SparseSet_bool* close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(close_fds); i++) {
        const size_t fd = SparseSet_bool_key_at_idx(close_fds, i);
        if (!SparseSet_HtttpEntry_contains(&data->entries, fd)) {
            continue;
        }

        HtttpParsedMessageQueue* pq = SparseSet_HtttpParsedMessageQueue_value_at(&data->parsed_qs, fd);
        HtttpParsedMessageQueue_free(pq);
        if (SparseSet_HtttpParsedMessageQueue_contains(&data->parsed_qs, fd)) {
            SparseSet_HtttpParsedMessageQueue_erase(&data->parsed_qs, fd);
        }

        HtttpOutboundMessageQueue* rq = SparseSet_HtttpOutboundMessageQueue_value_at(&data->response_qs, fd);
        response_queue_drain(rq);
        HtttpOutboundMessageQueue_free(rq);
        if (SparseSet_HtttpOutboundMessageQueue_contains(&data->response_qs, fd)) {
            SparseSet_HtttpOutboundMessageQueue_erase(&data->response_qs, fd);
        }

        memset(SparseSet_HtttpEntry_get(&data->entries, fd), 0, sizeof(HtttpEntry));
        SparseSet_HtttpEntry_erase(&data->entries, fd);
    }
}

void HtttpData_parse(HtttpData* data, const SparseSet_AuthFrameQueue* m_decrypt_qs,
                     SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
                     SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_AuthFrameQueue_size(m_decrypt_qs); i++) {
        const size_t fd = SparseSet_AuthFrameQueue_key_at_idx(m_decrypt_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_decrypt_qs fd must not be in err_fds");
            continue;
        }
        if (!SparseSet_HtttpEntry_contains(&data->entries, fd)) {
            assert(false && "m_decrypt_qs fd must have been accepted");
            continue;
        }
        AuthFrameQueue* q = SparseSet_AuthFrameQueue_at_idx(m_decrypt_qs, i);

        HtttpParsedMessageQueue* pq = SparseSet_HtttpParsedMessageQueue_value_at(m_parsed_qs, fd);
        const size_t pending = AuthFrameQueue_size(q);
        const size_t room = HtttpParsedMessageQueue_capacity(pq) - HtttpParsedMessageQueue_size(pq);
        // overflow violates the capacity contract (see server_tick's accept
        // fan-out); the fd is failed
        assert(pending <= room && "decrypt_qs and parsed_qs share cfg.client_capacity");
        if (pending > room) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            if (SparseSet_HtttpParsedMessageQueue_contains(m_parsed_qs, fd)) {
                HtttpParsedMessageQueue_reset(SparseSet_HtttpParsedMessageQueue_get(m_parsed_qs, fd));
                SparseSet_HtttpParsedMessageQueue_erase(m_parsed_qs, fd);
            }
            continue;
        }
        if (pending == 0) {
            continue;
        }

        HtttpParsedMessageQueue* out = SparseSet_HtttpParsedMessageQueue_activate(m_parsed_qs, fd);
        for (size_t j = 0; j < pending; j++) {
            const AuthFrame* frame = AuthFrameQueue_at(q, j);
            HtttpParsedMessage parsed;
            memset(&parsed, 0, sizeof(parsed));

            if (frame->status == AUTH_FRAME_OK && frame->frame.status == READER_FRAME_OK) {
                if (htttp_parse(frame->frame.content.ptr, frame->frame.content.length, &parsed.message) == 0) {
                    parsed.status = HTTTP_LAYER_PARSE_OK;
                }
                else {
                    memset(&parsed.message, 0, sizeof(parsed.message));
                    parsed.status = HTTTP_LAYER_PARSE_ERROR;
                }
            }
            else {
                parsed.status = HTTTP_LAYER_PARSE_ERROR;
            }

            const int err = HtttpParsedMessageQueue_push_back(out, &parsed);
            assert(err != -1);
            (void)err;
        }
    }
}

void HtttpData_respond_placeholder(HtttpData* data, const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
                                   SparseSet_HtttpOutboundMessageQueue* m_response_qs,
                                   SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_HtttpParsedMessageQueue_size(m_parsed_qs); i++) {
        const size_t fd = SparseSet_HtttpParsedMessageQueue_key_at_idx(m_parsed_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_parsed_qs fd must not be in err_fds");
            continue;
        }
        if (!SparseSet_HtttpEntry_contains(&data->entries, fd)) {
            assert(false && "m_parsed_qs fd must have been accepted");
            continue;
        }
        HtttpParsedMessageQueue* q = SparseSet_HtttpParsedMessageQueue_at_idx(m_parsed_qs, i);

        bool failed = false;
        const size_t count = HtttpParsedMessageQueue_size(q);
        HtttpOutboundMessageQueue* out = SparseSet_HtttpOutboundMessageQueue_activate(m_response_qs, fd);

        for (size_t j = 0; j < count; j++) {
            const HtttpParsedMessage* parsed = HtttpParsedMessageQueue_at(q, j);

            HtttpStatus status = HTTTP_STATUS_BAD_REQUEST;
            const char* body = NULL;
            size_t body_len = 0;
            if (parsed->status == HTTTP_LAYER_PARSE_OK && parsed->message.is_request) {
                status = HTTTP_STATUS_OK;
                body = (const char*)parsed->message.request.body;
                body_len = parsed->message.request.body_len;
            }

            HtttpOutboundMessage outbound;
            outbound.message.is_request = false;
            if (htttp_make_default_response(status, body, body_len, false,
                                            &outbound.message.response, &outbound.ownership) == -1) {
                failed = true;
                break;
            }

            const int err = HtttpOutboundMessageQueue_push_back(out, &outbound);
            assert(err != -1 && "parsed_qs and response_qs share cfg.client_capacity");
            if (err == -1) {
                htttp_message_free(&outbound.message, &outbound.ownership);
                failed = true;
                break;
            }
        }

        if (failed) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            if (SparseSet_HtttpOutboundMessageQueue_contains(m_response_qs, fd)) {
                response_queue_drain(SparseSet_HtttpOutboundMessageQueue_get(m_response_qs, fd));
                SparseSet_HtttpOutboundMessageQueue_erase(m_response_qs, fd);
            }
        }
    }
}

void HtttpData_serialize(HtttpData* data, const SparseSet_HtttpOutboundMessageQueue* m_response_qs,
                         SparseSet_AuthFrameQueue* m_auth_qs,
                         SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_HtttpOutboundMessageQueue_size(m_response_qs); i++) {
        const size_t fd = SparseSet_HtttpOutboundMessageQueue_key_at_idx(m_response_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_response_qs fd must not be in err_fds");
            continue;
        }
        if (!SparseSet_HtttpEntry_contains(&data->entries, fd)) {
            assert(false && "m_response_qs fd must have been accepted");
            continue;
        }
        HtttpOutboundMessageQueue* q = SparseSet_HtttpOutboundMessageQueue_at_idx(m_response_qs, i);

        bool failed = false;
        const size_t count = HtttpOutboundMessageQueue_size(q);
        AuthFrameQueue* out = SparseSet_AuthFrameQueue_activate(m_auth_qs, fd);
        
        for (size_t j = 0; j < count; j++) {
            const HtttpOutboundMessage* outbound = HtttpOutboundMessageQueue_at(q, j);

            size_t serialized_len = 0;
            unsigned char* serialized = htttp_serialize(&outbound->message, &serialized_len);
            if (serialized == NULL) {
                failed = true;
                break;
            }
            if (serialized_len == 0 || serialized_len > FRAME_MAX) {
                free(serialized);
                failed = true;
                break;
            }

            const AuthFrame response_frame = {
                { { serialized, serialized_len }, READER_FRAME_OK },
                AUTH_FRAME_OK,
            };
            const int err = AuthFrameQueue_push_back(out, &response_frame);
            assert(err != -1 && "response_qs and auth_qs share cfg.client_capacity");
            if (err == -1) {
                free(serialized);
                failed = true;
                break;
            }
        }

        if (failed) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            if (SparseSet_AuthFrameQueue_contains(m_auth_qs, fd)) {
                auth_queue_drain(SparseSet_AuthFrameQueue_get(m_auth_qs, fd));
                SparseSet_AuthFrameQueue_erase(m_auth_qs, fd);
            }
        }
    }
}
