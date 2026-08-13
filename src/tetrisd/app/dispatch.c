#include "app/dispatch.h"
#include "app/room.h"
#include "htttp.h"
#include "logger.h"
#include "proto.h"
#include "tetrisbrain/input.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    METHOD_JOIN = 0,
    METHOD_CREATE,
    METHOD_START,
    METHOD_MOVE,
    METHOD_ROTATE,
    METHOD_DROP,
    METHOD_HOLD,
    METHOD_LEAVE,
    METHOD_SET_PLAYER_NAME,
    METHOD_WHOAMI,
    METHOD_NOT_ALLOWED,
} Method;

static const char* METHOD_TABLE[] = {
    [METHOD_JOIN] = "JOIN",
    [METHOD_CREATE] = "CREATE",
    [METHOD_START] = "START",
    [METHOD_MOVE] = "MOVE",
    [METHOD_ROTATE] = "ROTATE",
    [METHOD_DROP] = "DROP",
    [METHOD_HOLD] = "HOLD",
    [METHOD_LEAVE] = "LEAVE",
    [METHOD_SET_PLAYER_NAME] = "SET_PLAYER_NAME",
    [METHOD_WHOAMI] = "WHOAMI",
};

static Method get_method(const char* method) {
    for (Method m = 0; m < METHOD_NOT_ALLOWED; m++) {
        if (strcmp(method, METHOD_TABLE[m]) == 0) {
            return m;
        }
    }
    return METHOD_NOT_ALLOWED;
}

/*!
    @brief build a response of @p status carrying @p body , a literal or NULL
*/
static DispatchResult respond(HtttpOutboundMessage* outbound, HtttpStatus status, const char* body) {
    const size_t body_len = body == NULL ? 0 : strlen(body);
    if (htttp_make_default_response(status, body, body_len, false, &outbound->message.response, &outbound->ownership) == -1) {
        return DISPATCH_ERR;
    }
    return DISPATCH_RESPOND;
}

static DispatchResult handle_method_not_allowed(HtttpOutboundMessage* outbound) {
    return respond(outbound, HTTTP_STATUS_METHOD_NOT_ALLOWED, NULL);
}

//! @brief the record of @p fd , which the precondition guarantees exists
static Player* get_player(const AppData* data, Fd fd) {
    assert(fd >= 0 && SparseSet_Player_contains(&data->players, (size_t)fd));
    return SparseSet_Player_get(&data->players, (size_t)fd);
}

//! @brief the room @p fd is in, or @c ROOM_IDX_NONE
static size_t get_room_idx(const AppData* data, Fd fd) {
    return get_player(data, fd)->room_idx;
}

static DispatchResult handle_room_create(AppData* data, Fd fd, HtttpOutboundMessage* outbound) {
    if (get_room_idx(data, fd) != ROOM_IDX_NONE) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Already in a room");
    }

    size_t room_idx;
    if (room_create(data, fd, &room_idx) == -1) {
        return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Room list full");
    }
    return respond(outbound, HTTTP_STATUS_CREATED, NULL);
}

static DispatchResult handle_room_start(AppData* data, Fd fd, HtttpOutboundMessage* outbound) {
    const size_t room_idx = get_room_idx(data, fd);
    if (room_idx == ROOM_IDX_NONE) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Not in a room");
    }
    if (SparseSet_Room_get(&data->rooms, room_idx)->status == ROOM_IN_GAME) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Game already started");
    }

    room_start(data, room_idx);
    return respond(outbound, HTTTP_STATUS_OK, NULL);
}

static DispatchResult handle_room_leave(AppData* data, Fd fd, HtttpOutboundMessage* outbound) {
    if (get_room_idx(data, fd) == ROOM_IDX_NONE) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Not in a room");
    }

    room_leave(data, fd);
    return respond(outbound, HTTTP_STATUS_OK, NULL);
}

/*!
    @brief copy @p parsed 's body into @p name as a player name

    A name is 1 to @c PLAYER_NAME_MAX printable ASCII bytes. Control bytes
    are refused because a name is echoed back by @c WHOAMI and written to
    the log, so neither would survive them intact.

    @post on failure @p name is unmodified

    @return -1 if the body is not a name, 0 otherwise
*/
static int parse_player_name(const HtttpRequest* parsed, char name[PLAYER_NAME_MAX + 1]) {
    if (parsed->body == NULL || parsed->body_len == 0 || parsed->body_len > PLAYER_NAME_MAX) {
        return -1;
    }
    for (size_t i = 0; i < parsed->body_len; i++) {
        if (parsed->body[i] < 0x20 || parsed->body[i] > 0x7E) {
            return -1;
        }
    }

    memcpy(name, parsed->body, parsed->body_len);
    name[parsed->body_len] = '\0';
    return 0;
}

/*!
    @brief build a 200 carrying @p name

    Both naming methods answer with the stored name rather than an empty
    body, so a client learns what it is called from the response it already
    waits for.
*/
static DispatchResult respond_player_name(const char* name, HtttpOutboundMessage* outbound) {
    // the body outlives this call, and the record it comes from does not:
    // the player may be closed before the response is serialized.
    char* const body = strdup(name);
    if (body == NULL) {
        return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    // takes the body over on failure too, so there is nothing to release here
    if (htttp_make_default_response(HTTTP_STATUS_OK, body, strlen(body), true,
                                    &outbound->message.response, &outbound->ownership) == -1) {
        return DISPATCH_ERR;
    }
    return DISPATCH_RESPOND;
}

/*!
    @brief name @p fd , or rename it

    Naming is independent of the room lifecycle: a player may be named
    before joining a room and renamed at any point in a game, since nothing
    below keys off the name.
*/
static DispatchResult handle_set_player_name(AppData* data, Fd fd, const HtttpRequest* parsed,
                                             HtttpOutboundMessage* outbound) {
    char name[PLAYER_NAME_MAX + 1];
    if (parse_player_name(parsed, name) == -1) {
        return respond(outbound, HTTTP_STATUS_BAD_REQUEST,
                       "Name must be 1 to 20 printable ASCII bytes");
    }

    memcpy(get_player(data, fd)->name, name, sizeof(name));
    LOGGER_LOG(LOG_INFO, "player", "fd=%d renamed to %s", fd, name);
    return respond_player_name(name, outbound);
}

//! @brief report the name of @p fd , if it has one
static DispatchResult handle_whoami(const AppData* data, Fd fd, HtttpOutboundMessage* outbound) {
    const char* const name = get_player(data, fd)->name;
    if (name[0] == '\0') {
        return respond(outbound, HTTTP_STATUS_NOT_FOUND, "No name set");
    }
    return respond_player_name(name, outbound);
}

/*!
    @brief set the key @p parsed 's body names in @p inputs

    Keys already set are left alone, so inputs of one tick accumulate.

    @pre @p method is @c METHOD_MOVE , @c METHOD_ROTATE , @c METHOD_DROP or
         @c METHOD_HOLD

    @post on failure @p inputs is unmodified

    @return -1 if the body does not encode a token of @p method , 0 otherwise
*/
static int parse_input(Method method, const HtttpRequest* parsed, bool (*inputs)[PLAYER_INPUT_KEY_COUNT]) {
    switch (method) {
    case METHOD_MOVE: {
        ProtoMoveRequest move;
        if (proto_parse_move_request(parsed->body, parsed->body_len, &move) == -1) {
            return -1;
        }
        (*inputs)[move == PROTO_MOVE_LEFT ? PLAYER_INPUT_KEY_MOVE_LEFT : PLAYER_INPUT_KEY_MOVE_RIGHT] = true;
        return 0;
    }
    case METHOD_ROTATE: {
        ProtoRotateRequest rotate;
        if (proto_parse_rotate_request(parsed->body, parsed->body_len, &rotate) == -1) {
            return -1;
        }
        (*inputs)[rotate == PROTO_ROTATE_CW ? PLAYER_INPUT_KEY_ROTATE_RIGHT : PLAYER_INPUT_KEY_ROTATE_LEFT] = true;
        return 0;
    }
    case METHOD_DROP: {
        ProtoDropRequest drop;
        if (proto_parse_drop_request(parsed->body, parsed->body_len, &drop) == -1) {
            return -1;
        }
        (*inputs)[drop == PROTO_DROP_SOFT ? PLAYER_INPUT_KEY_SOFT_DROP : PLAYER_INPUT_KEY_LOCK_DOWN] = true;
        return 0;
    }
    // the key is the whole request; the body carries nothing and is ignored
    case METHOD_HOLD:
        (*inputs)[PLAYER_INPUT_KEY_HOLD] = true;
        return 0;
    default:
        assert(false && "not an input method");
        return -1;
    }
}

/*!
    @brief record the input @p parsed carries in @p fd 's room, for the
           next tick to apply

    An input never gets a response, so a request from a player with no
    running game, and one whose body does not parse, are both dropped
    silently.
*/
static DispatchResult handle_input(AppData* data, Fd fd, Method method, const HtttpRequest* parsed) {
    const size_t room_idx = get_room_idx(data, fd);
    if (room_idx == ROOM_IDX_NONE) {
        return DISPATCH_NO_RESPONSE;
    }

    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    if (room->status != ROOM_IN_GAME) {
        return DISPATCH_NO_RESPONSE;
    }

    // an unparseable body has no response to be reported in
    (void)parse_input(method, parsed, &room->inputs);
    return DISPATCH_NO_RESPONSE;
}

DispatchResult respond_one_request(AppData* data, Fd fd, const HtttpRequest* parsed, HtttpOutboundMessage* outbound) {
    outbound->message.is_request = false;
    const Method method = get_method(parsed->method);
    switch (method) {
    case METHOD_JOIN:
    case METHOD_CREATE:
        return handle_room_create(data, fd, outbound);
    case METHOD_START:
        return handle_room_start(data, fd, outbound);
    case METHOD_MOVE:
    case METHOD_ROTATE:
    case METHOD_DROP:
    case METHOD_HOLD:
        return handle_input(data, fd, method, parsed);
    case METHOD_LEAVE:
        return handle_room_leave(data, fd, outbound);
    case METHOD_SET_PLAYER_NAME:
        return handle_set_player_name(data, fd, parsed, outbound);
    case METHOD_WHOAMI:
        return handle_whoami(data, fd, outbound);
    case METHOD_NOT_ALLOWED:
        return handle_method_not_allowed(outbound);
    default:
        assert(false);
        return DISPATCH_ERR;
    }
}
