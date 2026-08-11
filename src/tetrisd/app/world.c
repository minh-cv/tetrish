#include "app/world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    Bodies are short lines of `key=value`; the client prints them verbatim, so
    nothing here needs a parser on the other side.
*/
#define REPLY_BODY_MAX 256

int world_init(World* world, size_t player_capacity) {
    world->players = calloc(player_capacity, sizeof(*world->players));
    if (world->players == NULL) {
        return -1;
    }
    world->player_capacity = player_capacity;
    return 0;
}

void world_free(World* world) {
    free(world->players);
    world->players = NULL;
    world->player_capacity = 0;
}

PlayerRef world_accept(World* world, size_t index) {
    if (index >= world->player_capacity) {
        return player_ref_null();
    }

    Player* const player = &world->players[index];
    // slots start at generation 0, so the first live generation is 1 and the
    // reserved null generation is never handed out
    player->generation++;
    if (player->generation == APP_REF_NULL_GENERATION) {
        player->generation++;
    }
    player->present = true;
    snprintf(player->name, sizeof(player->name), "player%zu", index);

    const PlayerRef ref = {(uint32_t)index, player->generation};
    return ref;
}

void world_close(World* world, PlayerRef ref) {
    Player* const player = world_player(world, ref);
    if (player == NULL) {
        return;
    }
    player->present = false;
    player->generation++;
    if (player->generation == APP_REF_NULL_GENERATION) {
        player->generation++;
    }
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

/*
    Names go back out inside a reply body and, later, inside broadcasts to
    other players, so anything that could be mistaken for framing is refused
    rather than escaped.
*/
static bool is_usable_name(const char* body, size_t len) {
    if (len == 0 || len >= PLAYER_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)body[i];
        if (c <= ' ' || c > '~') {
            return false;
        }
    }
    return true;
}

static int reply_text(AppEffectSink* sink, PlayerRef target, HtttpStatus status, const char* text) {
    return app_effect_reply(sink, target, status, text, strlen(text));
}

static int apply_set_name(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    Player* const player = world_player(world, actor);

    if (!is_usable_name(command->body, command->body_len)) {
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

    char body[REPLY_BODY_MAX];
    const int written = snprintf(body, sizeof(body), "id=%u\nname=%s\n", actor.index, player->name);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return reply_text(sink, actor, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "cannot format reply\n");
    }
    return app_effect_reply(sink, actor, HTTTP_STATUS_OK, body, (size_t)written);
}

int world_apply(World* world, PlayerRef actor, const AppCommand* command, AppEffectSink* sink) {
    switch (command->kind) {
    case APP_COMMAND_SET_NAME:
        return apply_set_name(world, actor, command, sink);
    case APP_COMMAND_WHOAMI:
        return apply_whoami(world, actor, sink);
    case APP_COMMAND_JOIN:
    case APP_COMMAND_LEAVE:
    case APP_COMMAND_START:
    case APP_COMMAND_MOVE:
    case APP_COMMAND_ROTATE:
    case APP_COMMAND_DROP:
        // no room exists yet, so every room-scoped path is genuinely absent
        return reply_text(sink, actor, HTTTP_STATUS_NOT_FOUND, "no such room\n");
    }
    return reply_text(sink, actor, HTTTP_STATUS_INTERNAL_SERVER_ERROR, "unroutable command\n");
}
