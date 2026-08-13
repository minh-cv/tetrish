#include "app/room.h"
#include "logger.h"
#include "tetrisbrain/control.h"
#include "tetrisbrain/state.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

RoomConfig room_config_default(void) {
    const RoomConfig config = {
        .is_public = false,
        .max_players = 1,
        .cross_room_garbage = false,
        .shared_seed = true,
        .max_preview = ROOM_PREVIEW_MAX,
        .brain = state_config_default(),
    };
    return config;
}

RoomMember* room_find_member(Room* room, Fd fd) {
    for (size_t i = 0; i < room->member_count; i++) {
        if (room->members[i].fd == fd) {
            return &room->members[i];
        }
    }
    return NULL;
}

int room_create(AppData* data, Fd fd, const RoomConfig* config, size_t* out_room_idx) {
    assert(fd >= 0 && SparseSet_Player_contains(&data->players, (size_t)fd));

    Player* player = SparseSet_Player_get(&data->players, (size_t)fd);
    if (player->room_idx != ROOM_IDX_NONE) {
        return -1;
    }
    if (Vec_RoomIdx_size(&data->free_room_idxs) == 0) {
        return -1;
    }

    const size_t room_idx = *Vec_RoomIdx_back(&data->free_room_idxs);
    Room room;
    memset(&room, 0, sizeof(room));
    room.config = config == NULL ? room_config_default() : *config;
    assert(room.config.max_players >= 1 &&
           room.config.max_players <= data->max_players_per_room);
    room.members = &data->member_pool[room_idx * data->max_players_per_room];
    memset(room.members, 0, data->max_players_per_room * sizeof(RoomMember));
    room.members[0].fd = fd;
    room.member_count = 1;
    room.status = ROOM_LOBBY;

    const int err = SparseSet_Room_insert(&data->rooms, room_idx, &room);
    assert(err != -1 && "a free room key is not in rooms");
    if (err == -1) {
        return -1;
    }

    Vec_RoomIdx_pop_back(&data->free_room_idxs);
    player->room_idx = room_idx;
    *out_room_idx = room_idx;
    LOGGER_LOG(LOG_INFO, "room", "room=%zu created by fd=%d", room_idx, fd);
    return 0;
}

void room_start(AppData* data, size_t room_idx) {
    assert(SparseSet_Room_contains(&data->rooms, room_idx));

    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    const uint64_t room_seed = (uint64_t)time(NULL) ^ (uint64_t)room_idx * 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < room->member_count; i++) {
        RoomMember* member = &room->members[i];
        const uint64_t seed = room->config.shared_seed
            ? room_seed
            : room_seed ^ ((uint64_t)member->fd + 1) * 0x9e3779b97f4a7c15ULL;
        member->game = init_state(seed, &room->config.brain);
        const bool is_topped_out = apply_spawn(&member->game);
        assert(!is_topped_out && "an empty board cannot block the first piece");
        (void)is_topped_out;
        memset(member->inputs, 0, sizeof(member->inputs));
        member->alive = true;
    }
    room->alive_count = room->member_count;
    room->started_member_count = room->member_count;
    room->status = ROOM_IN_GAME;
    *SparseSet_bool_activate(&data->in_game_rooms, room_idx) = true;
    LOGGER_LOG(LOG_INFO, "room", "room=%zu game started with %zu members, seed=%llu",
               room_idx, room->member_count, (unsigned long long)room_seed);
}

void room_tick(AppData* data, size_t room_idx) {
    assert(SparseSet_Room_contains(&data->rooms, room_idx));

    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    assert(room->status == ROOM_IN_GAME);

    for (size_t i = 0; i < room->member_count; i++) {
        RoomMember* member = &room->members[i];
        if (!member->alive) {
            continue;
        }

        const bool (*inputs)[PLAYER_INPUT_KEY_COUNT] = (const bool (*)[PLAYER_INPUT_KEY_COUNT])&member->inputs;
        const bool is_topped_out = apply_player_inputs(&member->game, inputs);
        memset(member->inputs, 0, sizeof(member->inputs));
        if (is_topped_out) {
            member->alive = false;
            room->alive_count--;
            LOGGER_LOG(LOG_INFO, "room", "room=%zu fd=%d topped out, score=%d",
                       room_idx, member->fd, member->game.score);
        }
    }

    if (room->alive_count == 0) {
        room_end(data, room_idx);
    }
}

void room_end(AppData* data, size_t room_idx) {
    assert(SparseSet_Room_contains(&data->rooms, room_idx));

    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    room->status = ROOM_LOBBY;
    if (SparseSet_bool_contains(&data->in_game_rooms, room_idx)) {
        SparseSet_bool_erase(&data->in_game_rooms, room_idx);
    }
    LOGGER_LOG(LOG_INFO, "room", "room=%zu game over with %zu members",
               room_idx, room->member_count);
}

void room_leave(AppData* data, Fd fd) {
    assert(fd >= 0 && SparseSet_Player_contains(&data->players, (size_t)fd));

    Player* player = SparseSet_Player_get(&data->players, (size_t)fd);
    if (player->room_idx == ROOM_IDX_NONE) {
        return;
    }

    const size_t room_idx = player->room_idx;
    player->room_idx = ROOM_IDX_NONE;

    assert(SparseSet_Room_contains(&data->rooms, room_idx));
    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    RoomMember* member = room_find_member(room, fd);
    assert(member != NULL && "a player's room_idx names a room seating them");

    if (room->status == ROOM_IN_GAME && member->alive) {
        room->alive_count--;
    }

    *member = room->members[room->member_count - 1];
    room->member_count--;
    memset(&room->members[room->member_count], 0, sizeof(RoomMember));

    if (room->member_count == 0) {
        if (SparseSet_bool_contains(&data->in_game_rooms, room_idx)) {
            SparseSet_bool_erase(&data->in_game_rooms, room_idx);
        }
        LOGGER_LOG(LOG_INFO, "room", "room=%zu closed, fd=%d left", room_idx, fd);
        memset(room, 0, sizeof(Room));
        SparseSet_Room_erase(&data->rooms, room_idx);

        const int err = Vec_RoomIdx_push_back(&data->free_room_idxs, &room_idx);
        assert(err != -1 && "free list holds one entry per room key");
        (void)err;
        return;
    }

    LOGGER_LOG(LOG_INFO, "room", "room=%zu fd=%d left, %zu members remain",
               room_idx, fd, room->member_count);
    if (room->status == ROOM_IN_GAME && room->alive_count == 0) {
        room_end(data, room_idx);
    }
}
