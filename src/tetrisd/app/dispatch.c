#include "app/dispatch.h"
#include "app/room.h"
#include "app/util.h"
#include "cJSON.h"
#include "htttp.h"
#include "logger.h"
#include "proto.h"
#include "tetrisbrain/input.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
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
    METHOD_GET_ROOM_LIST,
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
    [METHOD_GET_ROOM_LIST] = "GET_ROOM_LIST",
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

/*!
    @brief build a response of @p status carrying @p body , heap-allocated
           and handed over, or NULL for a failed allocation

    @c htttp_make_default_response takes the body over on failure too, so
    no path out of here leaves it to release.
*/
static DispatchResult respond_heap(HtttpOutboundMessage* outbound, HtttpStatus status, char* body) {
    if (body == NULL) {
        return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    if (htttp_make_default_response(status, body, strlen(body), true,
                                    &outbound->message.response, &outbound->ownership) == -1) {
        return DISPATCH_ERR;
    }
    return DISPATCH_RESPOND;
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

//! @brief read an integral @p item within `[min, max]` into @p out
static int json_read_int(const cJSON* item, int min, int max, int* out) {
    if (!cJSON_IsNumber(item)) {
        return -1;
    }
    const double value = item->valuedouble;
    if (value < (double)min || value > (double)max) {
        return -1;
    }
    const int as_int = (int)value;
    if ((double)as_int != value) {
        return -1;
    }
    *out = as_int;
    return 0;
}

/*!
    @brief read @p parsed 's JSON body into @p out

    An absent or empty body is @c room_config_default() . Every key is
    optional; an unknown key, a wrong type or an out-of-range value fails
    with the key's name (a literal) in @p out_bad_key , and a body that is
    not a JSON object at all fails with @p out_bad_key NULL.

    @post on failure @p out is unspecified

    @return -1 on failure, 0 otherwise
*/
static int parse_room_config(const AppData* data, const HtttpRequest* parsed,
                             RoomConfig* out, const char** out_bad_key) {
    *out = room_config_default();
    *out_bad_key = NULL;
    if (parsed->body == NULL || parsed->body_len == 0) {
        return 0;
    }

    cJSON* root = cJSON_ParseWithLength((const char*)parsed->body, parsed->body_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    int result = 0;
    int max_players = 1;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, root) {
        const char* key = item->string;
        // out_bad_key gets these literals, never key: key lives in root
        if (strcmp(key, "public") == 0) {
            if (!cJSON_IsBool(item)) {
                *out_bad_key = "public";
            }
            out->is_public = cJSON_IsTrue(item);
        }
        else if (strcmp(key, "max_players") == 0) {
            if (json_read_int(item, 1, (int)data->max_players_per_room, &max_players) == -1) {
                *out_bad_key = "max_players";
            }
            out->max_players = (size_t)max_players;
        }
        else if (strcmp(key, "cross_room_garbage") == 0) {
            if (!cJSON_IsBool(item)) {
                *out_bad_key = "cross_room_garbage";
            }
            out->cross_room_garbage = cJSON_IsTrue(item);
        }
        else if (strcmp(key, "shared_seed") == 0) {
            if (!cJSON_IsBool(item)) {
                *out_bad_key = "shared_seed";
            }
            out->shared_seed = cJSON_IsTrue(item);
        }
        else if (strcmp(key, "max_preview") == 0) {
            if (json_read_int(item, 0, ROOM_PREVIEW_MAX, &out->max_preview) == -1) {
                *out_bad_key = "max_preview";
            }
        }
        else if (strcmp(key, "start_level") == 0) {
            if (json_read_int(item, 0, 99, &out->brain.start_level) == -1) {
                *out_bad_key = "start_level";
            }
        }
        else if (strcmp(key, "frames_per_level_up") == 0) {
            if (json_read_int(item, 0, 1000000000, &out->brain.frames_per_level_up) == -1) {
                *out_bad_key = "frames_per_level_up";
            }
        }
        else if (strcmp(key, "lock_delay_frames") == 0) {
            if (json_read_int(item, 1, 10000, &out->brain.lock_counter_max) == -1) {
                *out_bad_key = "lock_delay_frames";
            }
        }
        else if (strcmp(key, "lock_moves") == 0) {
            if (json_read_int(item, 0, 10000, &out->brain.lock_movement_counter_max) == -1) {
                *out_bad_key = "lock_moves";
            }
        }
        else {
            *out_bad_key = "an unknown key";
        }

        if (*out_bad_key != NULL) {
            result = -1;
            break;
        }
    }
    cJSON_Delete(root);
    return result;
}

static DispatchResult handle_room_create(AppData* data, Fd fd, const HtttpRequest* parsed,
                                         HtttpOutboundMessage* outbound) {
    if (get_room_idx(data, fd) != ROOM_IDX_NONE) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Already in a room");
    }

    RoomConfig config;
    const char* bad_key = NULL;
    if (parse_room_config(data, parsed, &config, &bad_key) == -1) {
        if (bad_key == NULL) {
            return respond(outbound, HTTTP_STATUS_BAD_REQUEST, "Body must be a JSON object");
        }
        return respond_heap(outbound, HTTTP_STATUS_BAD_REQUEST,
                            malloc_sprintf("Invalid %s", bad_key));
    }

    size_t room_idx;
    if (room_create(data, fd, &config, &room_idx) == -1) {
        return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Room list full");
    }
    // the checkpoint flow is telling friends the room code, so hand it back
    return respond_heap(outbound, HTTTP_STATUS_CREATED, malloc_sprintf("%zu", room_idx));
}

//! @brief read a path of exactly @p prefix then digits into @p out_idx
static int parse_path_idx(const char* path, const char* prefix, size_t* out_idx) {
    const size_t prefix_len = strlen(prefix);
    if (path == NULL || strncmp(path, prefix, prefix_len) != 0) {
        return -1;
    }
    const char* digits = path + prefix_len;
    if (*digits == '\0') {
        return -1;
    }
    size_t value = 0;
    for (const char* p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        const size_t digit = (size_t)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) {
            return -1;
        }
        value = value * 10 + digit;
    }
    *out_idx = value;
    return 0;
}

static DispatchResult handle_room_join(AppData* data, Fd fd, const HtttpRequest* parsed,
                                       HtttpOutboundMessage* outbound) {
    size_t room_idx;
    if (parse_path_idx(parsed->path, "/room/", &room_idx) == -1) {
        return respond(outbound, HTTTP_STATUS_BAD_REQUEST, "Path must be /room/<id>");
    }

    switch (room_join(data, fd, room_idx)) {
    case ROOM_JOIN_OK:
        return respond_heap(outbound, HTTTP_STATUS_OK, malloc_sprintf("%zu", room_idx));
    case ROOM_JOIN_ALREADY_IN_ROOM:
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Already in a room");
    case ROOM_JOIN_NO_SUCH_ROOM:
        return respond(outbound, HTTTP_STATUS_NOT_FOUND, "No such room");
    case ROOM_JOIN_FULL:
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Room full");
    case ROOM_JOIN_IN_GAME:
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Game in progress");
    default:
        assert(false);
        return DISPATCH_ERR;
    }
}

static DispatchResult handle_room_start(AppData* data, Fd fd, HtttpOutboundMessage* outbound) {
    const size_t room_idx = get_room_idx(data, fd);
    if (room_idx == ROOM_IDX_NONE) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Not in a room");
    }
    if (SparseSet_Room_get(&data->rooms, room_idx)->status == ROOM_IN_GAME) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Game already started");
    }

    if (room_start(data, room_idx) == -1) {
        return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Cannot start game");
    }
    return respond(outbound, HTTTP_STATUS_OK, NULL);
}

static DispatchResult handle_room_leave(AppData* data, Fd fd, HtttpOutboundMessage* outbound) {
    if (get_room_idx(data, fd) == ROOM_IDX_NONE) {
        return respond(outbound, HTTTP_STATUS_CONFLICT, "Not in a room");
    }

    room_leave(data, fd);
    return respond(outbound, HTTTP_STATUS_OK, NULL);
}

//! @brief rooms per GET_ROOM_LIST page, per the course spec's listing shape
#define ROOM_LIST_PAGE_SIZE 20

/*!
    @brief answer `GET_ROOM_LIST /rooms/<page>` with public rooms
           `[page * 20, (page + 1) * 20)` as a JSON array

    A page counts public rooms, not room keys. Slicing the key space first
    and filtering after would make most pages empty and some short: keys are
    handed out from the free list's back, so the first room created is the
    last key, and a browser asking for the first page would be told there is
    nothing to join while rooms sat on page 25.

    Rooms are visited in key order rather than through an index of the public
    ones. An index would make this cheaper, but @c SparseSet reorders its
    dense array on erase, so page boundaries would shift whenever any room
    closed — a browser would see one room twice and miss another. Key order
    is stable, and scanning @c max_rooms keys is not a cost worth trading it
    for.

    Private rooms are left out rather than marked: they are reachable by
    telling someone the room code, not by browsing. An empty page is a
    valid, empty array.
*/
static DispatchResult handle_get_room_list(AppData* data, const HtttpRequest* parsed,
                                           HtttpOutboundMessage* outbound) {
    size_t page_idx;
    if (parse_path_idx(parsed->path, "/rooms/", &page_idx) == -1) {
        return respond(outbound, HTTTP_STATUS_BAD_REQUEST, "Path must be /rooms/<page>");
    }

    cJSON* rooms = cJSON_CreateArray();
    if (rooms == NULL) {
        return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    // a page past SIZE_MAX / 20 cannot be reached by any count of rooms, so
    // it is simply empty; saturating keeps the multiply from wrapping
    const size_t first = page_idx > SIZE_MAX / ROOM_LIST_PAGE_SIZE
        ? SIZE_MAX
        : page_idx * ROOM_LIST_PAGE_SIZE;

    size_t ordinal = 0;
    size_t emitted = 0;
    for (size_t key = 0; key < data->rooms.capacity && emitted < ROOM_LIST_PAGE_SIZE; key++) {
        if (!SparseSet_Room_contains(&data->rooms, key)) {
            continue;
        }
        const Room* room = SparseSet_Room_get(&data->rooms, key);
        if (!room->config.is_public) {
            continue;
        }
        if (ordinal++ < first) {
            continue;
        }

        cJSON* entry = cJSON_CreateObject();
        if (entry == NULL ||
            cJSON_AddNumberToObject(entry, "id", (double)key) == NULL ||
            cJSON_AddNumberToObject(entry, "players", (double)room->member_count) == NULL ||
            cJSON_AddNumberToObject(entry, "max_players", (double)room->config.max_players) == NULL ||
            cJSON_AddStringToObject(entry, "status",
                                    room->status == ROOM_IN_GAME ? "in-game" : "lobby") == NULL) {
            cJSON_Delete(entry);
            cJSON_Delete(rooms);
            return respond(outbound, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        cJSON_AddItemToArray(rooms, entry);
        emitted++;
    }

    char* const body = cJSON_PrintUnformatted(rooms);
    cJSON_Delete(rooms);
    return respond_heap(outbound, HTTTP_STATUS_OK, body);
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
    return respond_heap(outbound, HTTTP_STATUS_OK, strdup(name));
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

    RoomMember* member = room_find_member(room, fd);
    assert(member != NULL && "a player's room_idx names a room seating them");
    // the dead spectate; their frozen board takes no keys
    if (!member->alive) {
        return DISPATCH_NO_RESPONSE;
    }

    // an unparseable body has no response to be reported in
    (void)parse_input(method, parsed, &member->inputs);
    return DISPATCH_NO_RESPONSE;
}

DispatchResult respond_one_request(AppData* data, Fd fd, const HtttpRequest* parsed, HtttpOutboundMessage* outbound) {
    outbound->message.is_request = false;
    const Method method = get_method(parsed->method);
    switch (method) {
    case METHOD_JOIN:
        return handle_room_join(data, fd, parsed, outbound);
    case METHOD_CREATE:
        return handle_room_create(data, fd, parsed, outbound);
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
    case METHOD_GET_ROOM_LIST:
        return handle_get_room_list(data, parsed, outbound);
    case METHOD_NOT_ALLOWED:
        return handle_method_not_allowed(outbound);
    default:
        assert(false);
        return DISPATCH_ERR;
    }
}
