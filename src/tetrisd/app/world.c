#include "app/world.h"
#include "app/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    Bodies are short lines of `key=value`; the client prints them verbatim, so
    nothing here needs a parser on the other side.
*/
#define REPLY_BODY_MAX 256

int world_init(World* world, size_t player_capacity, size_t room_capacity) {
    world->players = calloc(player_capacity, sizeof(*world->players));
    if (world->players == NULL) {
        return -1;
    }
    world->rooms = calloc(room_capacity, sizeof(*world->rooms));
    if (world->rooms == NULL) {
        free(world->players);
        world->players = NULL;
        return -1;
    }
    world->player_capacity = player_capacity;
    world->room_capacity = room_capacity;
    return 0;
}

void world_free(World* world) {
    free(world->players);
    world->players = NULL;
    free(world->rooms);
    world->rooms = NULL;
    world->player_capacity = 0;
    world->room_capacity = 0;
}

/*
    Generation 0 is the null handle, so a bump that lands back on it is
    skipped rather than handed out.
*/
static uint32_t next_generation(uint32_t generation) {
    generation++;
    if (generation == APP_REF_NULL_GENERATION) {
        generation++;
    }
    return generation;
}

PlayerRef world_accept(World* world, size_t index) {
    if (index >= world->player_capacity) {
        return player_ref_null();
    }

    Player* const player = &world->players[index];
    player->generation = next_generation(player->generation);
    player->present = true;
    player->room = room_ref_null();
    player->seat = 0;
    snprintf(player->name, sizeof(player->name), "player%zu", index);

    const PlayerRef ref = {(uint32_t)index, player->generation};
    return ref;
}

Player* world_player(const World* world, PlayerRef ref) {
    if (player_ref_is_null(ref) || ref.index >= world->player_capacity) {
        return NULL;
    }
    Player* const player = &world->players[ref.index];
    if (!player->present || player->generation != ref.generation) {
        return NULL;
    }
    return player;
}

Room* world_room(const World* world, RoomRef ref) {
    if (room_ref_is_null(ref) || ref.index >= world->room_capacity) {
        return NULL;
    }
    Room* const room = &world->rooms[ref.index];
    if (!room->present || room->generation != ref.generation) {
        return NULL;
    }
    return room;
}

static RoomRef room_ref_of(const World* world, const Room* room) {
    const RoomRef ref = {(uint32_t)(room - world->rooms), room->generation};
    return ref;
}

static Room* find_room(const World* world, const char* code, size_t code_len) {
    for (size_t i = 0; i < world->room_capacity; i++) {
        Room* const room = &world->rooms[i];
        if (!room->present) {
            continue;
        }
        if (strlen(room->code) == code_len && memcmp(room->code, code, code_len) == 0) {
            return room;
        }
    }
    return NULL;
}

static void room_destroy(Room* room) {
    room->present = false;
    room->generation = next_generation(room->generation);
    room->seat_count = 0;
    room->status = ROOM_LOBBY;
}

/*
    Seats are kept dense so a broadcast is one pass over `seat_count`, which
    means a departure shifts everyone behind it down and their Player.seat has
    to follow.
*/
static void room_remove_seat(World* world, Room* room, uint8_t seat) {
    for (uint8_t i = seat; i + 1 < room->seat_count; i++) {
        room->occupants[i] = room->occupants[i + 1];
        room->seats[i] = room->seats[i + 1];
        Player* const moved = world_player(world, room->occupants[i]);
        if (moved != NULL) {
            moved->seat = i;
        }
    }
    room->seat_count--;

    if (room->seat_count == 0) {
        room_destroy(room);
        return;
    }
    // an empty host slot would make START unreachable for everyone left
    if (world_player(world, room->host) == NULL) {
        room->host = room->occupants[0];
    }
}

void world_close(World* world, PlayerRef ref) {
    Player* const player = world_player(world, ref);
    if (player == NULL) {
        return;
    }

    Room* const room = world_room(world, player->room);
    if (room != NULL) {
        room_remove_seat(world, room, player->seat);
    }

    player->room = room_ref_null();
    player->present = false;
    player->generation = next_generation(player->generation);
}

/*
    Names and room codes both travel inside line-oriented bodies, so anything
    that could be mistaken for framing is refused rather than escaped.
*/
static bool is_usable_token(const char* text, size_t len, size_t max) {
    if (len == 0 || len >= max) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c <= ' ' || c > '~') {
            return false;
        }
    }
    return true;
}

static int reply_text(AppEffectSink* sink, PlayerRef target, HtttpStatus status, const char* text) {
    return app_effect_reply(sink, target, status, text, strlen(text));
}

static int reply_room(AppEffectSink* sink, const World* world, const Room* room,
                      PlayerRef target, HtttpStatus status) {
    char body[APP_STATE_BODY_MAX];
    size_t body_len;
    if (app_event_encode_room(world, room, body, sizeof(body), &body_len) == -1) {
        return reply_text(sink, target, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "cannot encode room\n");
    }
    return app_effect_reply(sink, target, status, body, body_len);
}

/* ---- identity ---- */

static int apply_set_name(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    Player* const player = world_player(world, actor);

    if (!is_usable_token(command->body, command->body_len, PLAYER_NAME_MAX)) {
        return reply_text(sink, actor, HTTTP_STATUS_BAD_REQUEST,
                          "name must be 1 to 23 printable non-space characters\n");
    }

    memcpy(player->name, command->body, command->body_len);
    player->name[command->body_len] = '\0';

    char body[REPLY_BODY_MAX];
    const int written = snprintf(body, sizeof(body), "name=%s\n", player->name);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return reply_text(sink, actor, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "cannot format reply\n");
    }
    return app_effect_reply(sink, actor, HTTTP_STATUS_OK, body, (size_t)written);
}

static int apply_whoami(World* world, PlayerRef actor, AppEffectSink* sink) {
    const Player* const player = world_player(world, actor);
    const Room* const room = world_room(world, player->room);

    char body[REPLY_BODY_MAX];
    const int written = snprintf(body, sizeof(body), "id=%u\nname=%s\nroom=%s\n",
                                 actor.index, player->name, room == NULL ? "-" : room->code);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return reply_text(sink, actor, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "cannot format reply\n");
    }
    return app_effect_reply(sink, actor, HTTTP_STATUS_OK, body, (size_t)written);
}

/* ---- rooms ---- */

static Room* room_create(World* world, const char* code, size_t code_len, PlayerRef host) {
    for (size_t i = 0; i < world->room_capacity; i++) {
        Room* const room = &world->rooms[i];
        if (room->present) {
            continue;
        }

        memset(room->code, 0, sizeof(room->code));
        memcpy(room->code, code, code_len);
        room->generation = next_generation(room->generation);
        room->present = true;
        room->seat_count = 0;
        room->host = host;
        room->status = ROOM_LOBBY;
        room->frame = 0;
        return room;
    }
    return NULL;
}

static int apply_join(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    Player* const player = world_player(world, actor);

    if (!is_usable_token(command->room_id, command->room_id_len, ROOM_CODE_MAX)) {
        return reply_text(sink, actor, HTTTP_STATUS_BAD_REQUEST, "unusable room code\n");
    }
    if (world_room(world, player->room) != NULL) {
        return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "already in a room\n");
    }

    Room* room = find_room(world, command->room_id, command->room_id_len);
    const bool created = room == NULL;
    if (created) {
        room = room_create(world, command->room_id, command->room_id_len, actor);
        if (room == NULL) {
            return reply_text(sink, actor, HTTTP_STATUS_TOO_MANY_REQUESTS, "no room slot free\n");
        }
    }
    else {
        if (room->status != ROOM_LOBBY) {
            return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "game already started\n");
        }
        if (room->seat_count == ROOM_SEAT_MAX) {
            return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "room is full\n");
        }
    }

    const uint8_t seat = room->seat_count;
    room->occupants[seat] = actor;
    memset(&room->seats[seat], 0, sizeof(room->seats[seat]));
    room->seats[seat].state = init_state();
    room->seats[seat].alive = false;
    room->seat_count++;

    player->room = room_ref_of(world, room);
    player->seat = seat;

    return reply_room(sink, world, room, actor, created ? HTTTP_STATUS_CREATED : HTTTP_STATUS_OK);
}

static int apply_leave(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    Player* const player = world_player(world, actor);
    Room* const room = world_room(world, player->room);

    if (room == NULL) {
        return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "not in a room\n");
    }
    if (strlen(room->code) != command->room_id_len ||
        memcmp(room->code, command->room_id, command->room_id_len) != 0) {
        return reply_text(sink, actor, HTTTP_STATUS_NOT_FOUND, "not in that room\n");
    }

    room_remove_seat(world, room, player->seat);
    player->room = room_ref_null();
    player->seat = 0;

    return reply_text(sink, actor, HTTTP_STATUS_OK, "left\n");
}

static int apply_start(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    const Player* const player = world_player(world, actor);
    Room* const room = world_room(world, player->room);

    if (room == NULL || strlen(room->code) != command->room_id_len ||
        memcmp(room->code, command->room_id, command->room_id_len) != 0) {
        return reply_text(sink, actor, HTTTP_STATUS_NOT_FOUND, "not in that room\n");
    }
    if (!player_ref_eq(room->host, actor)) {
        return reply_text(sink, actor, HTTTP_STATUS_FORBIDDEN, "only the host can start\n");
    }
    if (room->status == ROOM_RUNNING) {
        return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "already running\n");
    }

    room->frame = 0;
    room->status = ROOM_RUNNING;
    for (uint8_t i = 0; i < room->seat_count; i++) {
        Seat* const seat = &room->seats[i];
        memset(seat->inputs, 0, sizeof(seat->inputs));
        seat->state = init_state();
        // apply_spawn reports "the piece cannot be placed", which on an empty
        // board cannot happen; it is still checked so the invariant that a
        // running seat has a live piece holds by construction
        seat->alive = !apply_spawn(&seat->state);
    }

    return reply_room(sink, world, room, actor, HTTTP_STATUS_OK);
}

/* ---- play ---- */

/*
    The spec fixes the body of each play method to a pair of words. Anything
    else is a 400 rather than a silently ignored input, since a client that
    sends the wrong word would otherwise appear to work.
*/
static bool body_is(const AppCommand* command, const char* word) {
    const size_t len = strlen(word);
    return command->body_len == len && memcmp(command->body, word, len) == 0;
}

static bool play_key(const AppCommand* command, PlayerInputKey* out) {
    switch (command->kind) {
    case APP_COMMAND_MOVE:
        if (body_is(command, "LEFT")) { *out = PLAYER_INPUT_KEY_MOVE_LEFT; return true; }
        if (body_is(command, "RIGHT")) { *out = PLAYER_INPUT_KEY_MOVE_RIGHT; return true; }
        return false;
    case APP_COMMAND_ROTATE:
        if (body_is(command, "CW")) { *out = PLAYER_INPUT_KEY_ROTATE_RIGHT; return true; }
        if (body_is(command, "CCW")) { *out = PLAYER_INPUT_KEY_ROTATE_LEFT; return true; }
        return false;
    case APP_COMMAND_DROP:
        if (body_is(command, "SOFT")) { *out = PLAYER_INPUT_KEY_SOFT_DROP; return true; }
        if (body_is(command, "HARD")) { *out = PLAYER_INPUT_KEY_LOCK_DOWN; return true; }
        return false;
    case APP_COMMAND_HOLD:
        *out = PLAYER_INPUT_KEY_HOLD;
        return true;
    default:
        return false;
    }
}

static int apply_play(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    const Player* const player = world_player(world, actor);
    Room* const room = world_room(world, player->room);

    if (room == NULL || strlen(room->code) != command->room_id_len ||
        memcmp(room->code, command->room_id, command->room_id_len) != 0) {
        return reply_text(sink, actor, HTTTP_STATUS_NOT_FOUND, "not in that room\n");
    }
    // the path names whose piece to move; moving someone else's is the one
    // authorization question in the play path
    if (command->player_id != -1 && command->player_id != (long)actor.index) {
        return reply_text(sink, actor, HTTTP_STATUS_FORBIDDEN, "not your seat\n");
    }
    if (room->status != ROOM_RUNNING) {
        return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "game is not running\n");
    }
    if (!room->seats[player->seat].alive) {
        return reply_text(sink, actor, HTTTP_STATUS_CONFLICT, "seat is out\n");
    }

    PlayerInputKey key;
    if (!play_key(command, &key)) {
        return reply_text(sink, actor, HTTTP_STATUS_BAD_REQUEST, "unusable input\n");
    }

    room->seats[player->seat].inputs[key] = true;
    return reply_text(sink, actor, HTTTP_STATUS_OK, "");
}

int world_apply(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    switch (command->kind) {
    case APP_COMMAND_SET_NAME:
        return apply_set_name(world, actor, command, sink);
    case APP_COMMAND_WHOAMI:
        return apply_whoami(world, actor, sink);
    case APP_COMMAND_JOIN:
        return apply_join(world, actor, command, sink);
    case APP_COMMAND_LEAVE:
        return apply_leave(world, actor, command, sink);
    case APP_COMMAND_START:
        return apply_start(world, actor, command, sink);
    case APP_COMMAND_MOVE:
    case APP_COMMAND_ROTATE:
    case APP_COMMAND_DROP:
    case APP_COMMAND_HOLD:
        return apply_play(world, actor, command, sink);
    }
    return reply_text(sink, actor, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "unroutable command\n");
}

/* ---- the frame step ---- */

/*
    Garbage a seat sent this frame is credited to the next seat round-robin,
    which is the single-room stand-in for the cross-room routing battle royale
    needs. apply_attack moves a negative balance on the source into a positive
    one on the target, so the direction of the transfer lives in the brain
    rather than here.
*/
static void deliver_garbage(Room* room) {
    if (room->seat_count < 2) {
        return;
    }
    for (uint8_t i = 0; i < room->seat_count; i++) {
        if (!room->seats[i].alive) {
            continue;
        }
        for (uint8_t step = 1; step < room->seat_count; step++) {
            const uint8_t target = (uint8_t)((i + step) % room->seat_count);
            if (room->seats[target].alive) {
                apply_attack(&room->seats[i].state, &room->seats[target].state);
                break;
            }
        }
    }
}

static void room_step(Room* room) {
    for (uint8_t i = 0; i < room->seat_count; i++) {
        Seat* const seat = &room->seats[i];
        if (!seat->alive) {
            continue;
        }
        // apply_player_inputs takes a pointer to a const array, which is a
        // distinct and incompatible type from a pointer to the array itself
        const bool (*inputs)[PLAYER_INPUT_KEY_COUNT] =
            (const bool (*)[PLAYER_INPUT_KEY_COUNT]) & seat->inputs;
        if (apply_player_inputs(&seat->state, inputs)) {
            seat->alive = false;
        }
        memset(seat->inputs, 0, sizeof(seat->inputs));
    }

    deliver_garbage(room);
    room->frame++;

    for (uint8_t i = 0; i < room->seat_count; i++) {
        if (room->seats[i].alive) {
            return;
        }
    }
    room->status = ROOM_ENDED;
}

static int broadcast_room(World* world, Room* room, AppEffectSink* sink) {
    char path[ROOM_CODE_MAX + 8];
    const int path_len = snprintf(path, sizeof(path), "/room/%s", room->code);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
        return -1;
    }

    for (uint8_t i = 0; i < room->seat_count; i++) {
        if (world_player(world, room->occupants[i]) == NULL) {
            continue;
        }

        char body[APP_STATE_BODY_MAX];
        size_t body_len;
        if (app_event_encode_state(world, room, i, body, sizeof(body), &body_len) == -1) {
            return -1;
        }
        // a snapshot supersedes any older one still queued, so the flush is
        // free to drop it rather than close a peer that has fallen behind
        if (app_effect_event(sink, room->occupants[i], "STATE", path, (size_t)path_len,
                             body, body_len, true) == -1) {
            return -1;
        }
    }
    return 0;
}

int world_tick(World* world, uint64_t frames, bool broadcast, AppEffectSink* sink) {
    int result = 0;

    for (size_t i = 0; i < world->room_capacity; i++) {
        Room* const room = &world->rooms[i];
        if (!room->present) {
            continue;
        }

        for (uint64_t f = 0; f < frames && room->status == ROOM_RUNNING; f++) {
            room_step(room);
        }
        // lobby and ended rooms broadcast too: the roster inside a snapshot is
        // how a client learns who joined, and STATE is the only message the
        // server is allowed to originate
        if (broadcast && broadcast_room(world, room, sink) == -1) {
            result = -1;
        }
    }
    return result;
}
