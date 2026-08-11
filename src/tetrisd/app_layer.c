#include "app_layer.h"
#include "app/command.h"
#include "htttp.h"
#include "logger.h"
#include "type.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

int AppData_init(AppData* data, size_t max_entries, size_t max_rooms,
                 size_t effect_capacity, size_t arena_capacity) {
    if (SparseSet_AppEntry_init(&data->entries, max_entries) == -1) {
        return -1;
    }
    if (world_init(&data->world, max_entries, max_rooms) == -1) {
        SparseSet_AppEntry_free(&data->entries);
        return -1;
    }
    if (app_effect_sink_init(&data->sink, effect_capacity, arena_capacity) == -1) {
        world_free(&data->world);
        SparseSet_AppEntry_free(&data->entries);
        return -1;
    }
    return 0;
}

void AppData_free(AppData* data) {
    app_effect_sink_free(&data->sink);
    world_free(&data->world);
    SparseSet_AppEntry_free(&data->entries);
}

void AppData_reset(AppData* data) {
    app_effect_sink_reset(&data->sink);
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
        entry.self = world_accept(&data->world, fd);
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

        LOGGER_LOG(LOG_INFO, "app", "fd=%zu disconnected", fd);
        AppEntry* const entry = SparseSet_AppEntry_get(&data->entries, fd);
        world_close(&data->world, entry->self);
        entry->self = player_ref_null();
        SparseSet_AppEntry_erase(&data->entries, fd);
    }
}

/*
    A frame that never became a message still deserves an answer, so the
    transport-level statuses map onto status codes here rather than being
    dropped.
*/
static HtttpStatus frame_status_code(FrameStatus status, const char** reason) {
    switch (status) {
    case FRAME_OK:
        *reason = "ok";
        return HTTTP_STATUS_OK;
    case FRAME_DECRYPT_ERROR:
        *reason = "cannot decrypt frame\n";
        return HTTTP_STATUS_BAD_REQUEST;
    case FRAME_PAYLOAD_TOO_LARGE:
        *reason = "payload too large\n";
        return HTTTP_STATUS_PAYLOAD_TOO_LARGE;
    case FRAME_HTTTP_PARSE_ERROR:
        *reason = "cannot parse request\n";
        return HTTTP_STATUS_BAD_REQUEST;
    }
    *reason = "unknown frame status\n";
    return HTTTP_STATUS_INTERNAL_SERVER_ERROR;
}

static int respond_one(AppData* data, PlayerRef actor, long self_id, const HtttpParsedMessage* parsed) {
    if (parsed->status != FRAME_OK) {
        const char* reason;
        const HtttpStatus status = frame_status_code(parsed->status, &reason);
        return app_effect_reply(&data->sink, actor, status, reason, strlen(reason));
    }

    // the spec requires every request to be logged; this is the one place a
    // request becomes visible as a request
    LOGGER_LOG(LOG_INFO, "app", "fd=%ld request %s %s", self_id,
               parsed->message.request.method, parsed->message.request.path);

    AppCommand command;
    const AppCommandStatus parse_status = app_command_parse(&parsed->message, self_id, &command);
    if (parse_status != APP_COMMAND_OK) {
        const char* const reason = app_command_status_reason(parse_status);
        LOGGER_LOG(LOG_INFO, "app", "fd=%ld rejected request: %s", self_id, reason);
        return app_effect_reply(&data->sink, actor, app_command_status_code(parse_status),
                                reason, strlen(reason));
    }

    return world_apply(&data->world, actor, &command, &data->sink);
}

void AppData_respond(AppData* data, const SparseSet_HtttpParsedMessageQueue* m_parsed_qs,
                     SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_HtttpParsedMessageQueue_size(m_parsed_qs); i++) {
        const size_t fd = SparseSet_HtttpParsedMessageQueue_key_at_idx(m_parsed_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_parsed_qs fd must not be in err_fds");
            continue;
        }
        if (!SparseSet_AppEntry_contains(&data->entries, fd)) {
            assert(false && "m_parsed_qs fd must have been accepted");
            continue;
        }

        const PlayerRef actor = SparseSet_AppEntry_get(&data->entries, fd)->self;
        HtttpParsedMessageQueue* const q = SparseSet_HtttpParsedMessageQueue_at_idx(m_parsed_qs, i);
        const size_t count = HtttpParsedMessageQueue_size(q);

        for (size_t j = 0; j < count; j++) {
            if (respond_one(data, actor, (long)fd, HtttpParsedMessageQueue_at(q, j)) == -1) {
                LOGGER_LOG(LOG_WARN, "app", "fd=%zu closed: no room to record its effects", fd);
                *SparseSet_bool_activate(err_fds, fd) = true;
                break;
            }
        }
    }
}

void AppData_tick(AppData* data, uint64_t frames, bool broadcast) {
    if (world_tick(&data->world, frames, broadcast, &data->sink) == -1) {
        // a dropped snapshot costs one frame of display, and the next one
        // carries everything it would have
        LOGGER_LOG(LOG_WARN, "app", "dropped a state broadcast: no room to record it");
    }
}

/*
    The spec puts Player-Id on authenticated traffic; attaching it to every
    outbound message is also how a client learns its own id without a
    dedicated round trip.
*/
static int attach_player_id(HtttpHeader* header, size_t* header_count, bool* is_value_owned, uint32_t id) {
    if (*header_count >= HTTTP_HEADER_MAX) {
        return -1;
    }

    char scratch[32];
    const int written = snprintf(scratch, sizeof(scratch), "%u", id);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        return -1;
    }

    char* const value = strdup(scratch);
    if (value == NULL) {
        return -1;
    }

    header[*header_count].key = "Player-Id";
    header[*header_count].value = value;
    is_value_owned[*header_count] = true;
    (*header_count)++;
    return 0;
}

static char* dup_body(const char* body, size_t body_len) {
    char* const copy = malloc(body_len == 0 ? 1 : body_len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, body, body_len);
    return copy;
}

static int build_reply(const AppEffectSink* sink, const AppEffect* effect,
                       uint32_t id, HtttpOutboundMessage* out) {
    char* const body = dup_body(app_effect_body(sink, effect), effect->body_len);
    if (body == NULL) {
        return -1;
    }

    out->message.is_request = false;
    // takes ownership of body, including on failure
    if (htttp_make_default_response(effect->status, body, effect->body_len, true,
                                    &out->message.response, &out->ownership) == -1) {
        return -1;
    }
    if (attach_player_id(out->message.response.header, &out->message.response.header_count,
                         out->ownership.is_value_owned, id) == -1) {
        htttp_message_free(&out->message, &out->ownership);
        return -1;
    }
    return 0;
}

/*
    Events are requests, not responses, so that a client can keep pairing its
    own responses with the requests it issued: an unsolicited response would
    break that correlation.
*/
static int build_event(const AppEffectSink* sink, const AppEffect* effect,
                       uint32_t id, HtttpOutboundMessage* out) {
    char* const path = malloc(effect->path_len + 1);
    if (path == NULL) {
        return -1;
    }
    memcpy(path, app_effect_path(sink, effect), effect->path_len);
    path[effect->path_len] = '\0';

    char* const body = dup_body(app_effect_body(sink, effect), effect->body_len);
    if (body == NULL) {
        free(path);
        return -1;
    }

    char length[32];
    const int written = snprintf(length, sizeof(length), "%zu", effect->body_len);
    if (written < 0 || (size_t)written >= sizeof(length)) {
        free(path);
        free(body);
        return -1;
    }
    char* const content_length = strdup(length);
    if (content_length == NULL) {
        free(path);
        free(body);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->message.is_request = true;
    out->message.request.method = effect->method;
    out->message.request.path = path;
    out->message.request.header[0].key = "Content-Length";
    out->message.request.header[0].value = content_length;
    out->message.request.header[1].key = "Content-Type";
    out->message.request.header[1].value = "application/tetris-state";
    out->message.request.header_count = 2;
    out->message.request.body = (const unsigned char*)body;
    out->message.request.body_len = effect->body_len;

    out->ownership.is_path_owned = true;
    out->ownership.is_value_owned[0] = true;
    out->ownership.is_body_owned = true;

    if (attach_player_id(out->message.request.header, &out->message.request.header_count,
                         out->ownership.is_value_owned, id) == -1) {
        htttp_message_free(&out->message, &out->ownership);
        return -1;
    }
    return 0;
}

/*
    Fan-out breaks the one-output-per-input contract the rest of the pipeline
    relies on, so a full queue is a normal condition here rather than an
    assertion. A snapshot is dropped, since the next one carries everything it
    would have; anything else is a delta the peer cannot be resynchronized to
    cheaply, so the connection goes.

    Activation happens before the push and is undone when the push leaves the
    queue empty, so the layer's "active iff nonempty" invariant holds on both
    paths.
*/
static bool queue_outbound(SparseSet_HtttpOutboundMessageQueue* m_response_qs, size_t fd,
                           const AppEffect* effect, HtttpOutboundMessage* message) {
    HtttpOutboundMessageQueue* const q = SparseSet_HtttpOutboundMessageQueue_activate(m_response_qs, fd);
    if (HtttpOutboundMessageQueue_push_back(q, message) == 0) {
        return true;
    }

    htttp_message_free(&message->message, &message->ownership);
    if (HtttpOutboundMessageQueue_empty(q)) {
        SparseSet_HtttpOutboundMessageQueue_erase(m_response_qs, fd);
    }
    return effect->coalescable;
}

void AppData_flush(AppData* data, SparseSet_HtttpOutboundMessageQueue* m_response_qs,
                   SparseSet_bool* err_fds) {
    for (size_t i = 0; i < data->sink.count; i++) {
        const AppEffect* const effect = &data->sink.effects[i];
        const size_t fd = effect->target.index;

        if (world_player(&data->world, effect->target) == NULL) {
            continue;
        }
        if (!SparseSet_AppEntry_contains(&data->entries, fd) ||
            SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }

        if (effect->kind == APP_EFFECT_REPLY) {
            LOGGER_LOG(LOG_INFO, "app", "fd=%zu response %d", fd, (int)effect->status);
        }
        else {
            LOGGER_LOG(LOG_DEBUG, "app", "fd=%zu event %s", fd, effect->method);
        }

        HtttpOutboundMessage message;
        const int built = effect->kind == APP_EFFECT_REPLY
                              ? build_reply(&data->sink, effect, effect->target.index, &message)
                              : build_event(&data->sink, effect, effect->target.index, &message);

        bool failed = built == -1;
        if (!failed) {
            failed = !queue_outbound(m_response_qs, fd, effect, &message);
        }

        if (failed) {
            LOGGER_LOG(LOG_WARN, "app", "fd=%zu closed: cannot deliver its outbound message", fd);
            *SparseSet_bool_activate(err_fds, fd) = true;
            if (SparseSet_HtttpOutboundMessageQueue_contains(m_response_qs, fd)) {
                response_queue_drain(SparseSet_HtttpOutboundMessageQueue_get(m_response_qs, fd));
                SparseSet_HtttpOutboundMessageQueue_erase(m_response_qs, fd);
            }
        }
    }
}
