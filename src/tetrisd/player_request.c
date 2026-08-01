#include "cJSON.h"
#include "player_request.h"
#include "dtor.h"
#include "htttp.h"
#include "logger.h"
#include "type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(free)

/*!
    @brief Build the standard response shape: status line plus the
    Content-Length/Content-Type/Player-Id/Date headers, the last formatted per
    spec as "p<number>".

    @param body a literal or a buffer that outlives serialization, or NULL
    with `body_len` 0. If `is_body_heap_allocated`, this function takes
    ownership of it, including on failure (mirrors `player_queue_frame`).
    @note on -1 `out`'s lifetime never began: nothing was attached, so the
    caller must not call `htttp_message_free`.
*/
static int player_make_default_response(int status, const char* reason, const char* body, size_t body_len, bool is_body_heap_allocated, HtttpResponse* out, HtttpMessageOwnership* own) {
    DTOR_DEFINE(errdtor, 5);
    DTOR_DEFINE(dtor, 1);

    if (is_body_heap_allocated) {
        DTOR_INSERT(errdtor, free, (void*)body);
    }

    char scratch[32];
    int written = snprintf(scratch, sizeof(scratch), "%zu", body_len);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    char* content_length = strdup(scratch);
    if (content_length == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, content_length);

    char* date = htttp_make_rfc_1123_date();
    if (date == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, date);

    HtttpResponse new_out = {
        status,
        reason,
        {
            {
                "Content-Length",
                content_length,
            },
            {
                "Content-Type",
                "text/plain",
            },
            {
                "Date",
                date,
            },
        },
        3,
        (const unsigned char*)body,
        body_len,
    };

    HtttpMessageOwnership new_own = {
        .is_value_owned[0] = true,
        .is_value_owned[2] = true,
        .is_body_owned = is_body_heap_allocated,
    };

    *out = new_out;
    *own = new_own;
    DTOR_RETURN(dtor, 0);
}

static bool player_is_name_valid(const char* name, size_t name_len) {
    if (name_len >= PLAYER_NAME_MAX) {
        return false;
    }


    for (size_t i = 0; i < name_len; i++) {
        char current = name[i];
        if (current < 32 || current >= 127) {
            return false;
        }
    }

    return true;
}

static int player_set_name(PlayerFdData* player, HtttpRequest* request, HtttpResponse* response, HtttpMessageOwnership* ownership) {
    cJSON* json_body = cJSON_ParseWithLength((char*)request->body, request->body_len);

    if (json_body == NULL) {
        static const char BODY[] = "Cannot parse JSON string from body";
        if (player_make_default_response(400, "Bad Request", BODY, sizeof(BODY) - 1, false, response, ownership) == -1) {
            return -1;
        }
        return 0;
    }

    const char* name = cJSON_GetStringValue(json_body);
    if (name == NULL) {
        cJSON_Delete(json_body);
        static const char BODY[] = "Cannot parse JSON string from body";
        if (player_make_default_response(400, "Bad Request", BODY, sizeof(BODY) - 1, false, response, ownership) == -1) {
            return -1;
        }
        return 0;
    }

    size_t name_len = strlen(name);
    if (!player_is_name_valid(name, name_len)) {
        cJSON_Delete(json_body);
        static const char INVALID_NAME_BODY[] = "Name contains invalid character";
        if (player_make_default_response(400, "Bad Request", INVALID_NAME_BODY, sizeof(INVALID_NAME_BODY) - 1, false, response, ownership) == -1) {
            return -1;
        }
        return 0;
    }

    memcpy(player->name, name, name_len);
    player->name[name_len] = '\0';

    cJSON_Delete(json_body);

    LOGGER_LOG(LOG_INFO, "set_name", "client fd=%d set name to %s", player->fd, player->name);

    if (player_make_default_response(200, "OK", NULL, 0, false, response, ownership) == -1) {
        return -1;
    }
    return 0;
}

// int player_list_rooms(HtttpRequest* request, Server* server, HtttpResponse* response, HtttpMessageOwnership* ownership) {
//     const char* body;
//     size_t body_len;
//     return player_make_default_response(200, "OK", body, body_len, true, response, ownership);
// }

// int player_join_request(PlayerFdData* player, HtttpRequest* request, Server* server, HtttpResponse* response, HtttpMessageOwnership* ownership) {
    
// }

int player_handle_request(PlayerFdData* player, HtttpRequest* request, Server* server, HtttpResponse* response, HtttpMessageOwnership* ownership) {
    (void)server;
    const char* method = request->method;
    if (strcmp(method, "SET_PLAYER_NAME") == 0) {
        return player_set_name(player, request, response, ownership);
    }
    // if (strcmp(method, "JOIN") == 0) {
    //     return player_join_request(player, request, server, response, ownership);
    // }

    if (player_make_default_response(405, "Method Not Allowed", NULL, 0, false, response, ownership) == -1) {
        return -1;
    }
    return 0;
}
