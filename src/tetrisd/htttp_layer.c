#include "htttp_layer.h"
#include "htttp.h"
#include "wire.h"
#include <assert.h>
#include <stdlib.h>

/*!
    @see auth_queue_drain (auth.c) — intentional duplicate
*/
static void response_queue_drain(AuthFrameQueue* q) {
    const size_t count = AuthFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const AuthFrame* frame = AuthFrameQueue_front(q);
        if (frame->status == AUTH_FRAME_OK && frame->frame.status == READER_FRAME_OK) {
            free(frame->frame.content.ptr);
        }
        AuthFrameQueue_pop_front(q);
    }
}

static unsigned char* make_response_bytes(HtttpStatus status, const unsigned char* body, size_t body_len, size_t* out_len) {
    HtttpMessage msg;
    HtttpMessageOwnership own;
    msg.is_request = false;
    if (htttp_make_default_response(status, (const char*)body, body_len, false, &msg.response, &own) == -1) {
        return NULL;
    }

    size_t len = 0;
    unsigned char* bytes = htttp_serialize(&msg, &len);
    htttp_message_free(&msg, &own);
    if (bytes == NULL) {
        return NULL;
    }
    if (len == 0 || len > FRAME_MAX) {
        free(bytes);
        return NULL;
    }
    *out_len = len;
    return bytes;
}

static int respond_frame(const AuthFrame* frame, size_t fd, SparseSet_AuthFrameQueue* m_auth_qs) {
    if (frame->status != AUTH_FRAME_OK || frame->frame.status != READER_FRAME_OK) {
        // error-status frames travel in-band; encrypt, the last layer before
        // the socket, decides the close. They carry no content, so this copy
        // shares no ownership with decrypt_qs.
        AuthFrameQueue* out = SparseSet_AuthFrameQueue_activate(m_auth_qs, fd);
        const int err = AuthFrameQueue_push_back(out, frame);
        assert(err != -1);
        return err;
    }

    HtttpMessage request;
    HtttpStatus status = HTTTP_STATUS_OK;
    const unsigned char* body = NULL;
    size_t body_len = 0;
    if (htttp_parse(frame->frame.content.ptr, frame->frame.content.length, &request) == -1 ||
        !request.is_request) {
        status = HTTTP_STATUS_BAD_REQUEST;
    }
    else {
        body = request.request.body;
        body_len = request.request.body_len;
    }

    size_t response_len = 0;
    unsigned char* response = make_response_bytes(status, body, body_len, &response_len);
    if (response == NULL && body_len > 0) {
        // the echoed body may have pushed the response past FRAME_MAX (the
        // failure cause is not distinguishable from allocation failure, but
        // retrying bodyless is harmless either way)
        response = make_response_bytes(HTTTP_STATUS_PAYLOAD_TOO_LARGE, NULL, 0, &response_len);
    }
    if (response == NULL) {
        return -1;
    }

    const AuthFrame out_frame = {
        { { response, response_len }, READER_FRAME_OK },
        AUTH_FRAME_OK,
    };
    AuthFrameQueue* out = SparseSet_AuthFrameQueue_activate(m_auth_qs, fd);
    const int err = AuthFrameQueue_push_back(out, &out_frame);
    // one output frame per input frame and both queues share a capacity, so
    // the push cannot fail
    assert(err != -1);
    if (err == -1) {
        free(response);
        return -1;
    }
    return 0;
}

void Htttp_respond(const SparseSet_AuthFrameQueue* m_decrypt_qs,
                   SparseSet_AuthFrameQueue* m_auth_qs,
                   SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_AuthFrameQueue_size(m_decrypt_qs); i++) {
        const size_t fd = SparseSet_AuthFrameQueue_key_at_idx(m_decrypt_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_decrypt_qs fd must not be in err_fds");
            continue;
        }
        AuthFrameQueue* q = SparseSet_AuthFrameQueue_at_idx(m_decrypt_qs, i);

        bool failed = false;
        const size_t frame_count = AuthFrameQueue_size(q);
        for (size_t j = 0; j < frame_count; j++) {
            if (respond_frame(AuthFrameQueue_at(q, j), fd, m_auth_qs) == -1) {
                failed = true;
                break;
            }
        }

        if (failed) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            if (SparseSet_AuthFrameQueue_contains(m_auth_qs, fd)) {
                response_queue_drain(SparseSet_AuthFrameQueue_get(m_auth_qs, fd));
                SparseSet_AuthFrameQueue_erase(m_auth_qs, fd);
            }
        }
    }
}
