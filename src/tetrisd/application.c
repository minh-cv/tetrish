#include "application.h"

#include "htttp.h"
#include "logger.h"
#include "wire.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_closing(const SparseSet_bool* close_fds, size_t fd) {
    return SparseSet_bool_contains(close_fds, fd);
}

static void mark_closing(SparseSet_bool* close_fds, size_t fd) {
    *SparseSet_bool_activate(close_fds, fd) = true;
}

static int queue_plaintext(
    SparseSet_AuthFrameQueue* response_qs,
    size_t fd,
    unsigned char* plaintext,
    size_t plaintext_length,
    AuthFrameStatus status
) {
    if (plaintext == NULL || plaintext_length == 0 || plaintext_length > FRAME_MAX) {
        free(plaintext);
        return -1;
    }
    const AuthFrame frame = {
        .frame = {
            .content = {.ptr = plaintext, .length = plaintext_length},
            .status = READER_FRAME_OK,
        },
        .status = status,
    };
    AuthFrameQueue* queue = SparseSet_AuthFrameQueue_activate(response_qs, fd);
    if (AuthFrameQueue_push_back(queue, &frame) == -1) {
        free(plaintext);
        return -1;
    }
    return 0;
}

static unsigned char* serialize_response(
    HtttpStatus status,
    const void* body,
    size_t body_length,
    size_t* serialized_length
) {
    HtttpMessage message;
    HtttpMessageOwnership ownership;
    memset(&message, 0, sizeof(message));
    memset(&ownership, 0, sizeof(ownership));
    if (htttp_make_default_response(
        status,
        body,
        body_length,
        false,
        &message.response,
        &ownership
    ) == -1) {
        return NULL;
    }
    message.is_request = false;
    unsigned char* serialized = htttp_serialize(&message, serialized_length);
    htttp_message_free(&message, &ownership);
    return serialized;
}

static int queue_response(
    SparseSet_AuthFrameQueue* response_qs,
    size_t fd,
    HtttpStatus status,
    const void* body,
    size_t body_length
) {
    size_t serialized_length = 0;
    unsigned char* serialized = serialize_response(
        status,
        body,
        body_length,
        &serialized_length
    );
    return queue_plaintext(
        response_qs,
        fd,
        serialized,
        serialized_length,
        AUTH_FRAME_OK
    );
}

static bool valid_player_name(const unsigned char* name, size_t length) {
    if (length == 0 || length > SERVER_PLAYER_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = name[i];
        if (!(isalnum(ch) || ch == ' ' || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static size_t format_identity(
    char* destination,
    size_t capacity,
    size_t fd,
    const ServerPlayer* player
) {
    const int written = snprintf(
        destination,
        capacity,
        "{\"id\":%zu,\"name\":\"%s\"}",
        fd,
        player->name
    );
    return written < 0 || (size_t)written >= capacity ? 0 : (size_t)written;
}

static int dispatch_request(
    ServerApplication* application,
    size_t fd,
    const HtttpMessage* request,
    SparseSet_AuthFrameQueue* response_qs
) {
    static const char BAD_REQUEST[] = "invalid request";
    static const char METHOD_NOT_ALLOWED[] = "unsupported method";
    if (!request->is_request) {
        return queue_response(
            response_qs,
            fd,
            HTTTP_STATUS_BAD_REQUEST,
            BAD_REQUEST,
            sizeof(BAD_REQUEST) - 1
        );
    }

    ServerPlayer* player = SparseSet_ServerPlayer_get(&application->players, fd);
    const HtttpRequest* value = &request->request;
    if (strcmp(value->method, "HTTTP") == 0) {
        return queue_response(
            response_qs,
            fd,
            HTTTP_STATUS_OK,
            value->body,
            value->body_len
        );
    }
    if (strcmp(value->method, "SET_PLAYER_NAME") == 0) {
        if (!valid_player_name(value->body, value->body_len)) {
            return queue_response(
                response_qs,
                fd,
                HTTTP_STATUS_BAD_REQUEST,
                BAD_REQUEST,
                sizeof(BAD_REQUEST) - 1
            );
        }
        memcpy(player->name, value->body, value->body_len);
        player->name[value->body_len] = '\0';
        LOGGER_LOG(LOG_INFO, "application", "fd=%zu renamed to %s", fd, player->name);
    } else if (strcmp(value->method, "WHOAMI") != 0) {
        return queue_response(
            response_qs,
            fd,
            HTTTP_STATUS_METHOD_NOT_ALLOWED,
            METHOD_NOT_ALLOWED,
            sizeof(METHOD_NOT_ALLOWED) - 1
        );
    }

    char identity[128];
    const size_t identity_length = format_identity(
        identity,
        sizeof(identity),
        fd,
        player
    );
    return identity_length == 0 ? -1 : queue_response(
        response_qs,
        fd,
        HTTTP_STATUS_OK,
        identity,
        identity_length
    );
}

int ServerApplication_init(ServerApplication* application, size_t max_entries) {
    if (SparseSet_ServerPlayer_init(&application->players, max_entries) == -1) {
        return -1;
    }
    application->state_sequence = 0;
    return 0;
}

void ServerApplication_free(ServerApplication* application) {
    SparseSet_ServerPlayer_free(&application->players);
    application->state_sequence = 0;
}

void ServerApplication_sync_authenticated(
    ServerApplication* application,
    const AuthData* auth,
    const SparseSet_bool* close_fds
) {
    for (size_t i = 0; i < SparseSet_AuthEntry_size(&auth->entries); ++i) {
        const size_t fd = SparseSet_AuthEntry_key_at_idx(&auth->entries, i);
        if (!AuthData_is_authenticated(auth, fd) || is_closing(close_fds, fd) ||
            SparseSet_ServerPlayer_contains(&application->players, fd)) {
            continue;
        }
        ServerPlayer player;
        const int written = snprintf(
            player.name,
            sizeof(player.name),
            "player-%zu",
            fd
        );
        assert(written > 0 && (size_t)written < sizeof(player.name));
        if (SparseSet_ServerPlayer_insert(&application->players, fd, &player) == -1) {
            assert(false && "authenticated player must fit in fd table");
        } else {
            LOGGER_LOG(LOG_INFO, "application", "authenticated fd=%zu", fd);
        }
    }
}

void ServerApplication_handle_requests(
    ServerApplication* application,
    const SparseSet_AuthFrameQueue* decrypted,
    SparseSet_AuthFrameQueue* response_qs,
    SparseSet_bool* close_fds
) {
    for (size_t i = 0; i < SparseSet_AuthFrameQueue_size(decrypted); ++i) {
        const size_t fd = SparseSet_AuthFrameQueue_key_at_idx(decrypted, i);
        if (is_closing(close_fds, fd)) {
            continue;
        }
        if (!SparseSet_ServerPlayer_contains(&application->players, fd)) {
            mark_closing(close_fds, fd);
            continue;
        }

        const AuthFrameQueue* queue = SparseSet_AuthFrameQueue_at_idx(decrypted, i);
        for (size_t j = 0; j < AuthFrameQueue_size(queue); ++j) {
            const AuthFrame* frame = AuthFrameQueue_at(queue, j);
            if (frame->status == AUTH_FRAME_DECRYPT_FAILURE) {
                mark_closing(close_fds, fd);
                break;
            }
            if (frame->frame.status == READER_FRAME_PAYLOAD_TOO_LARGE) {
                static const char TOO_LARGE[] = "payload too large";
                if (queue_response(
                    response_qs,
                    fd,
                    HTTTP_STATUS_PAYLOAD_TOO_LARGE,
                    TOO_LARGE,
                    sizeof(TOO_LARGE) - 1
                ) == -1) {
                    mark_closing(close_fds, fd);
                }
                continue;
            }

            HtttpMessage request;
            memset(&request, 0, sizeof(request));
            if (htttp_parse(
                frame->frame.content.ptr,
                frame->frame.content.length,
                &request
            ) == -1) {
                static const char INVALID_HTTTP[] = "invalid HTTTP";
                if (queue_response(
                    response_qs,
                    fd,
                    HTTTP_STATUS_BAD_REQUEST,
                    INVALID_HTTTP,
                    sizeof(INVALID_HTTTP) - 1
                ) == -1) {
                    mark_closing(close_fds, fd);
                    break;
                }
                continue;
            }
            if (dispatch_request(application, fd, &request, response_qs) == -1) {
                mark_closing(close_fds, fd);
                break;
            }
        }
    }
}

static unsigned char* serialize_state(
    const ServerApplication* application,
    size_t fd,
    size_t* serialized_length
) {
    const ServerPlayer* player = SparseSet_ServerPlayer_get(
        &application->players,
        fd
    );
    char body[256];
    const int body_written = snprintf(
        body,
        sizeof(body),
        "{\"sequence\":%llu,\"id\":%zu,\"name\":\"%s\",\"players\":%zu}",
        (unsigned long long)application->state_sequence,
        fd,
        player->name,
        SparseSet_ServerPlayer_size(&application->players)
    );
    if (body_written < 0 || (size_t)body_written >= sizeof(body)) {
        return NULL;
    }
    char content_length[32];
    const int length_written = snprintf(
        content_length,
        sizeof(content_length),
        "%d",
        body_written
    );
    if (length_written < 0 || (size_t)length_written >= sizeof(content_length)) {
        return NULL;
    }
    const HtttpMessage message = {
        .request = {
            .method = "STATE",
            .path = "",
            .header = {
                {"Content-Length", content_length},
                {"Content-Type", "application/tetris-state+json"},
            },
            .header_count = 2,
            .body = (const unsigned char*)body,
            .body_len = (size_t)body_written,
        },
        .is_request = true,
    };
    return htttp_serialize(&message, serialized_length);
}

void ServerApplication_push_state(
    ServerApplication* application,
    const AuthData* auth,
    const PlayerIo* io,
    uint64_t expirations,
    SparseSet_AuthFrameQueue* response_qs,
    const SparseSet_bool* close_fds
) {
    application->state_sequence += expirations;
    for (size_t i = 0; i < SparseSet_ServerPlayer_size(&application->players); ++i) {
        const size_t fd = SparseSet_ServerPlayer_key_at_idx(&application->players, i);
        if (is_closing(close_fds, fd) || !AuthData_is_authenticated(auth, fd) ||
            !PlayerIo_output_idle(io, fd)) {
            continue;
        }
        if (SparseSet_AuthFrameQueue_contains(response_qs, fd) &&
            !AuthFrameQueue_empty(SparseSet_AuthFrameQueue_get(response_qs, fd))) {
            continue;
        }
        size_t serialized_length = 0;
        unsigned char* serialized = serialize_state(
            application,
            fd,
            &serialized_length
        );
        if (queue_plaintext(
            response_qs,
            fd,
            serialized,
            serialized_length,
            AUTH_FRAME_STATE_PUSH
        ) == -1) {
            LOGGER_LOG(LOG_DEBUG, "application", "STATE skipped for fd=%zu", fd);
        }
    }
}

void ServerApplication_close(
    ServerApplication* application,
    const SparseSet_bool* close_fds
) {
    for (size_t i = 0; i < SparseSet_bool_size(close_fds); ++i) {
        const size_t fd = SparseSet_bool_key_at_idx(close_fds, i);
        if (SparseSet_ServerPlayer_contains(&application->players, fd)) {
            LOGGER_LOG(LOG_INFO, "application", "closing fd=%zu", fd);
            SparseSet_ServerPlayer_erase(&application->players, fd);
        }
    }
}
