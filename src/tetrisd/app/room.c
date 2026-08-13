#include "app/room.h"
#include "logger.h"
#include "tetrisbrain/control.h"
#include "tetrisbrain/state.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

int room_create(AppData* data, Fd fd, size_t* out_room_idx) {
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
    room.member = fd;
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
    const uint64_t seed = (uint64_t)time(NULL) ^ (uint64_t)room_idx * 0x9e3779b97f4a7c15ULL;
    room->game = init_state(seed, NULL);
    const bool is_topped_out = apply_spawn(&room->game);
    assert(!is_topped_out && "an empty board cannot block the first piece");
    (void)is_topped_out;
    memset(room->inputs, 0, sizeof(room->inputs));
    room->status = ROOM_IN_GAME;
    *SparseSet_bool_activate(&data->in_game_rooms, room_idx) = true;
    LOGGER_LOG(LOG_INFO, "room", "room=%zu game started for fd=%d, seed=%llu",
               room_idx, room->member, (unsigned long long)seed);
}

void room_tick(AppData* data, size_t room_idx) {
    assert(SparseSet_Room_contains(&data->rooms, room_idx));

    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    assert(room->status == ROOM_IN_GAME);

    const bool (*inputs)[PLAYER_INPUT_KEY_COUNT] = (const bool (*)[PLAYER_INPUT_KEY_COUNT])&room->inputs;
    const bool is_topped_out = apply_player_inputs(&room->game, inputs);
    memset(room->inputs, 0, sizeof(room->inputs));
    if (is_topped_out) {
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
    LOGGER_LOG(LOG_INFO, "room", "room=%zu game over for fd=%d, score=%d",
               room_idx, room->member, room->game.score);
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
    assert(SparseSet_Room_get(&data->rooms, room_idx)->member == fd);

    if (SparseSet_bool_contains(&data->in_game_rooms, room_idx)) {
        SparseSet_bool_erase(&data->in_game_rooms, room_idx);
    }

    LOGGER_LOG(LOG_INFO, "room", "room=%zu closed, fd=%d left", room_idx, fd);
    memset(SparseSet_Room_get(&data->rooms, room_idx), 0, sizeof(Room));
    SparseSet_Room_erase(&data->rooms, room_idx);

    const int err = Vec_RoomIdx_push_back(&data->free_room_idxs, &room_idx);
    assert(err != -1 && "free list holds one entry per room key");
    (void)err;
}
