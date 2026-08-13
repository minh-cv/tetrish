#include "app/room.h"
#include "tetrisbrain/state.h"
#include <assert.h>
#include <string.h>

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
    return 0;
}

void room_start(AppData* data, size_t room_idx) {
    assert(SparseSet_Room_contains(&data->rooms, room_idx));

    Room* room = SparseSet_Room_get(&data->rooms, room_idx);
    room->game = init_state();
    room->status = ROOM_IN_GAME;
    *SparseSet_bool_activate(&data->in_game_rooms, room_idx) = true;
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

    memset(SparseSet_Room_get(&data->rooms, room_idx), 0, sizeof(Room));
    SparseSet_Room_erase(&data->rooms, room_idx);

    const int err = Vec_RoomIdx_push_back(&data->free_room_idxs, &room_idx);
    assert(err != -1 && "free list holds one entry per room key");
    (void)err;
}
