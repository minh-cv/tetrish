#include "app/command.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PATH_SHAPE_PLAYER,        // /player
    PATH_SHAPE_ROOM,          // /room/<id>
    PATH_SHAPE_ROOM_PLAYER,   // /room/<id>/player/<pid>
} PathShape;

typedef struct {
    const char* method;
    AppCommandKind kind;
    PathShape shape;
} MethodEntry;

/*
    A table rather than a strcmp chain: the live-extension task pool is
    explicitly about adding a method under time pressure, and a row is the
    whole change here.
*/
static const MethodEntry METHOD_TABLE[] = {
    {"SET_PLAYER_NAME", APP_COMMAND_SET_NAME, PATH_SHAPE_PLAYER},
    {"WHOAMI", APP_COMMAND_WHOAMI, PATH_SHAPE_PLAYER},
    {"JOIN", APP_COMMAND_JOIN, PATH_SHAPE_ROOM},
    {"LEAVE", APP_COMMAND_LEAVE, PATH_SHAPE_ROOM},
    {"START", APP_COMMAND_START, PATH_SHAPE_ROOM},
    {"MOVE", APP_COMMAND_MOVE, PATH_SHAPE_ROOM_PLAYER},
    {"ROTATE", APP_COMMAND_ROTATE, PATH_SHAPE_ROOM_PLAYER},
    {"DROP", APP_COMMAND_DROP, PATH_SHAPE_ROOM_PLAYER},
};

static const size_t METHOD_TABLE_COUNT = sizeof(METHOD_TABLE) / sizeof(METHOD_TABLE[0]);

/*
    Matches a literal prefix and advances past it.
*/
static bool eat(const char** p, const char* literal) {
    const size_t len = strlen(literal);
    if (strncmp(*p, literal, len) != 0) {
        return false;
    }
    *p += len;
    return true;
}

/*
    A path segment is everything up to the next '/' or the end. Empty segments
    are rejected so `/room//player/3` cannot pass as a room named "".
*/
static bool eat_segment(const char** p, const char** out, size_t* out_len) {
    const char* const start = *p;
    const char* q = start;
    while (*q != '\0' && *q != '/') {
        q++;
    }
    if (q == start) {
        return false;
    }
    *out = start;
    *out_len = (size_t)(q - start);
    *p = q;
    return true;
}

static bool parse_long_segment(const char* start, size_t len, long* out) {
    char scratch[32];
    if (len == 0 || len >= sizeof(scratch)) {
        return false;
    }
    memcpy(scratch, start, len);
    scratch[len] = '\0';

    char* end;
    const long value = strtol(scratch, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_path(const char* path, PathShape shape, AppCommand* out) {
    const char* p = path;

    switch (shape) {
    case PATH_SHAPE_PLAYER:
        return eat(&p, "/player") && *p == '\0';

    case PATH_SHAPE_ROOM:
        return eat(&p, "/room/") && eat_segment(&p, &out->room_id, &out->room_id_len) && *p == '\0';

    case PATH_SHAPE_ROOM_PLAYER: {
        if (!eat(&p, "/room/") || !eat_segment(&p, &out->room_id, &out->room_id_len)) {
            return false;
        }
        if (!eat(&p, "/player/")) {
            return false;
        }
        const char* id_start;
        size_t id_len;
        if (!eat_segment(&p, &id_start, &id_len) || *p != '\0') {
            return false;
        }
        return parse_long_segment(id_start, id_len, &out->player_id);
    }
    }
    return false;
}

AppCommandStatus app_command_parse(const HtttpMessage* message, long self_id, AppCommand* out) {
    if (!message->is_request) {
        return APP_COMMAND_ERR_NOT_A_REQUEST;
    }

    const MethodEntry* entry = NULL;
    for (size_t i = 0; i < METHOD_TABLE_COUNT; i++) {
        if (strcmp(message->request.method, METHOD_TABLE[i].method) == 0) {
            entry = &METHOD_TABLE[i];
            break;
        }
    }
    if (entry == NULL) {
        return APP_COMMAND_ERR_UNKNOWN_METHOD;
    }

    const char* const claimed = htttp_get_header(message, "Player-Id");
    if (claimed != NULL) {
        long claimed_id;
        if (!parse_long_segment(claimed, strlen(claimed), &claimed_id)) {
            return APP_COMMAND_ERR_WRONG_PLAYER;
        }
        // -1 is what a client sends before its first response told it the id
        if (claimed_id != -1 && claimed_id != self_id) {
            return APP_COMMAND_ERR_WRONG_PLAYER;
        }
    }

    AppCommand command = {0};
    command.kind = entry->kind;
    command.player_id = -1;
    command.body = message->request.body == NULL ? "" : (const char*)message->request.body;
    command.body_len = message->request.body == NULL ? 0 : message->request.body_len;

    if (!parse_path(message->request.path, entry->shape, &command)) {
        return APP_COMMAND_ERR_BAD_PATH;
    }

    *out = command;
    return APP_COMMAND_OK;
}

HtttpStatus app_command_status_code(AppCommandStatus status) {
    switch (status) {
    case APP_COMMAND_OK: return HTTTP_STATUS_OK;
    case APP_COMMAND_ERR_NOT_A_REQUEST: return HTTTP_STATUS_BAD_REQUEST;
    case APP_COMMAND_ERR_UNKNOWN_METHOD: return HTTTP_STATUS_NOT_FOUND;
    case APP_COMMAND_ERR_BAD_PATH: return HTTTP_STATUS_NOT_FOUND;
    case APP_COMMAND_ERR_WRONG_PLAYER: return HTTTP_STATUS_FORBIDDEN;
    }
    return HTTTP_STATUS_INTERNAL_SERVER_ERROR;
}

const char* app_command_status_reason(AppCommandStatus status) {
    switch (status) {
    case APP_COMMAND_OK: return "ok";
    case APP_COMMAND_ERR_NOT_A_REQUEST: return "expected a request, got a response";
    case APP_COMMAND_ERR_UNKNOWN_METHOD: return "unknown method";
    case APP_COMMAND_ERR_BAD_PATH: return "path does not fit the method";
    case APP_COMMAND_ERR_WRONG_PLAYER: return "Player-Id names another player";
    }
    return "unknown error";
}
