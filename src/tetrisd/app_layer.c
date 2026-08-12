#include "app_layer.h"
#include "htttp.h"
#include "type.h"
#include <assert.h>
#include <string.h>

/*!
    @see htttp_layer.c
*/
static void response_queue_drain(HtttpOutboundMessageQueue* q) {
    const size_t count = HtttpOutboundMessageQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        HtttpOutboundMessage* m = HtttpOutboundMessageQueue_front(q);
        htttp_message_free(&m->message, &m->ownership);
        HtttpOutboundMessageQueue_pop_front(q);
    }
}

int AppData_init(AppData* data, size_t max_entries) {
    return SparseSet_AppEntry_init(&data->entries, max_entries);
}

void AppData_free(AppData* data) {
    SparseSet_AppEntry_free(&data->entries);
}

void AppData_accept(AppData* data, const Vec_Fd* fds, SparseSet_bool* err_fds) {
    for (size_t i = 0; i < Vec_Fd_size(fds); i++) {
        const Fd fd_raw = *Vec_Fd_at(fds, i);
        assert(fd_raw >= 0 && (size_t)fd_raw < data->entries.capacity);
        const size_t fd = (size_t)fd_raw;

        if (SparseSet_AppEntry_contains(&data->entries, fd)) {
            assert(false && "accepted fd must not already be in entries");
            continue;
        }
        if (SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }

        AppEntry entry;
        memset(&entry, 0, sizeof(entry));
        const int err = SparseSet_AppEntry_insert(&data->entries, fd, &entry);
        assert(err != -1);
        (void)err;
    }
}

void AppData_close(AppData* data, const SparseSet_bool* close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(close_fds); i++) {
        const size_t fd = SparseSet_bool_key_at_idx(close_fds, i);
        if (!SparseSet_AppEntry_contains(&data->entries, fd)) {
            continue;
        }

        memset(SparseSet_AppEntry_get(&data->entries, fd), 0, sizeof(AppEntry));
        SparseSet_AppEntry_erase(&data->entries, fd);
    }
}

static int respond_one_request(AppData* data, Fd fd, const HtttpRequest* parsed, HtttpOutboundMessage* outbound) {
    (void)data;
    (void)fd;
    outbound->message.is_request = false;
    if (htttp_make_default_response(HTTTP_STATUS_OK, (const char*)parsed->body, parsed->body_len, false, &outbound->message.response, &outbound->ownership) == -1) {
        return -1;
    }
    return 0;
        }

static int respond_one_frame(AppData* data, Fd fd, const HtttpParsedMessage* parsed, HtttpOutboundMessage* outbound, bool* is_written) {
            HtttpStatus status = HTTTP_STATUS_BAD_REQUEST;
            const char* body = NULL;
            size_t body_len = 0;
            switch (parsed->status) {
            case FRAME_OK:
                if (parsed->message.is_request) {
            int out = respond_one_request(data, fd, &parsed->message.request, outbound);
            *is_written = out == 0;
            return out;
        }
        else {
            *is_written = false;
            return 0;
                }
                break;
            case FRAME_DECRYPT_ERROR:
                status = HTTTP_STATUS_BAD_REQUEST;
                body = "Cannot decrypt message";
                body_len = strlen(body);
                break;
            case FRAME_PAYLOAD_TOO_LARGE:
                status = HTTTP_STATUS_PAYLOAD_TOO_LARGE;
                body = "Payload too large";
                body_len = strlen(body);
                break;
            case FRAME_HTTTP_PARSE_ERROR:
                status = HTTTP_STATUS_BAD_REQUEST;
                body = "Cannot parse request";
                body_len = strlen(body);
                break;
            }

    outbound->message.is_request = false;
    if (htttp_make_default_response(status, body, body_len, false, &outbound->message.response, &outbound->ownership) == -1) {
        *is_written = false;
        return -1;
    }
    *is_written = true;
    return 0;
}

void AppData_respond(AppData* data, const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
                     SparseSet_HtttpOutboundMessageQueue* m_response_qs,
                     SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_HtttpParsedMessageQueue_size(m_parsed_qs); i++) {
        const size_t fd = SparseSet_HtttpParsedMessageQueue_key_at_idx(m_parsed_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_parsed_qs fd must not be in err_fds");
            continue;
        }
        if (!SparseSet_Player_contains(&data->players, fd)) {
            assert(false && "m_parsed_qs fd must have been accepted");
            continue;
        }
        HtttpParsedMessageQueue* q = SparseSet_HtttpParsedMessageQueue_at_idx(m_parsed_qs, i);

        bool failed = false;
        const size_t count = HtttpParsedMessageQueue_size(q);
        HtttpOutboundMessageQueue* out = SparseSet_HtttpOutboundMessageQueue_activate(m_response_qs, fd);

        for (size_t j = 0; j < count; j++) {
            const HtttpParsedMessage* parsed = HtttpParsedMessageQueue_at(q, j);

            HtttpOutboundMessage response = {
                .message.is_request = false,
            };
            bool is_written;

            if (respond_one_frame(data, (Fd)fd, parsed, &response, &is_written) == -1) {
                failed = true;
                break;
            }

            if (!is_written) {
                continue;
            }

            const int err = HtttpOutboundMessageQueue_push_back(out, &response);
            assert(err != -1 && "parsed_qs and response_qs share cfg.client_capacity");
            if (err == -1) {
                htttp_message_free(&response.message, &response.ownership);
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
