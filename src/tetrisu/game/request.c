#include "game/request.h"

#include "proto.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*!
    @brief render @p argument 's room options as a JSON object into @p scratch

    Options are recognised by shape, matching the router: a decimal word is
    the seat count and each remaining word names a flag.

    @return the body length, or `0` when there are no options to send
*/
static size_t build_create_body(const char* argument, GameRequestScratch* scratch) {
    if (argument == NULL || *argument == '\0') {
        return 0;
    }

    unsigned int max_players = 0;
    bool is_public = false;
    bool cross_room = false;
    const char* cursor = argument;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (*cursor >= '0' && *cursor <= '9') {
            max_players = 0;
            while (*cursor >= '0' && *cursor <= '9') {
                // a seat count wider than the daemon's cap is rejected there,
                // so clamping here would only hide the error
                if (max_players < 100000u) {
                    max_players = max_players * 10u + (unsigned int)(*cursor - '0');
                }
                ++cursor;
            }
            continue;
        }
        if (strncmp(cursor, "public", 6) == 0) {
            is_public = true;
            cursor += 6;
            continue;
        }
        if (strncmp(cursor, "cross", 5) == 0) {
            cross_room = true;
            cursor += 5;
            continue;
        }
        ++cursor;
    }

    int written = 0;
    if (max_players > 0) {
        written = snprintf(
            scratch->body, sizeof(scratch->body),
            "{\"max_players\":%u,\"public\":%s,\"cross_room_garbage\":%s}",
            max_players, is_public ? "true" : "false", cross_room ? "true" : "false"
        );
    }
    else {
        written = snprintf(
            scratch->body, sizeof(scratch->body),
            "{\"public\":%s,\"cross_room_garbage\":%s}",
            is_public ? "true" : "false", cross_room ? "true" : "false"
        );
    }
    return written > 0 && (size_t)written < sizeof(scratch->body) ? (size_t)written : 0;
}

int game_request_from_intent(
    GameIntentType intent,
    const char* argument,
    GameRequestScratch* scratch,
    ClientRequest* out
) {
    memset(out, 0, sizeof(*out));
    out->path = "/";
    out->content_type = "application/tetris-command";
    out->completion = CLIENT_REQUEST_EXPECT_REPLY;

    switch (intent) {
    case GAME_INTENT_CREATE: {
        out->method = "CREATE";
        const size_t body_len = build_create_body(argument, scratch);
        if (body_len > 0) {
            out->body = (const unsigned char*)scratch->body;
            out->body_len = body_len;
        }
        break;
    }
    case GAME_INTENT_ROOM_LIST: {
        out->method = "GET_ROOM_LIST";
        // the daemon pages the listing, twenty rooms at a time, and reads the
        // page from the path the way it reads a room id
        const int written = snprintf(
            scratch->path, sizeof(scratch->path), "/rooms/%s",
            argument == NULL || *argument == '\0' ? "0" : argument
        );
        if (written <= 0 || (size_t)written >= sizeof(scratch->path)) {
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->path = scratch->path;
        break;
    }
    case GAME_INTENT_JOIN: {
        out->method = "JOIN";
        // the room the player named; the daemon reads the id from the path
        const int written = snprintf(
            scratch->path, sizeof(scratch->path), "/room/%s",
            argument == NULL ? "" : argument
        );
        if (written <= 0 || (size_t)written >= sizeof(scratch->path)) {
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->path = scratch->path;
        break;
    }
    case GAME_INTENT_START:
        out->method = "START";
        break;
    case GAME_INTENT_MOVE_LEFT:
        out->method = "MOVE";
        out->body = proto_serialize_move_request(PROTO_MOVE_LEFT, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_MOVE_RIGHT:
        out->method = "MOVE";
        out->body = proto_serialize_move_request(PROTO_MOVE_RIGHT, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_ROTATE_CW:
        out->method = "ROTATE";
        out->body = proto_serialize_rotate_request(PROTO_ROTATE_CW, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_ROTATE_CCW:
        out->method = "ROTATE";
        out->body = proto_serialize_rotate_request(PROTO_ROTATE_CCW, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_DROP_SOFT:
        out->method = "DROP";
        out->body = proto_serialize_drop_request(PROTO_DROP_SOFT, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_DROP_HARD:
        out->method = "DROP";
        out->body = proto_serialize_drop_request(PROTO_DROP_HARD, &out->body_len);
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_HOLD:
        out->method = "HOLD";
        out->completion = CLIENT_REQUEST_COMPLETE_ON_SEND;
        break;
    case GAME_INTENT_LEAVE:
        out->method = "LEAVE";
        break;
    default:
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}
