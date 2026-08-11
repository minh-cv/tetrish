#include "app/event.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char CELL_CHAR[TETROMINO_CELL_COUNT] = {
    [TETROMINO_CELL_I] = 'I',
    [TETROMINO_CELL_J] = 'J',
    [TETROMINO_CELL_L] = 'L',
    [TETROMINO_CELL_O] = 'O',
    [TETROMINO_CELL_S] = 'S',
    [TETROMINO_CELL_T] = 'T',
    [TETROMINO_CELL_Z] = 'Z',
    [TETROMINO_CELL_EMPTY] = '.',
    [TETROMINO_CELL_GARBAGE] = '#',
};

static const char TYPE_CHAR[TETROMINO_TYPE_COUNT] = {'I', 'J', 'L', 'O', 'S', 'T', 'Z'};

static const char* status_name(RoomStatus status) {
    switch (status) {
    case ROOM_LOBBY: return "lobby";
    case ROOM_RUNNING: return "running";
    case ROOM_ENDED: return "ended";
    }
    return "unknown";
}

/*
    A cursor over a fixed buffer. Every append checks what is left, so a body
    that outgrows the buffer is reported once at the end rather than truncated
    silently into something the client would parse as valid.
*/
typedef struct {
    char* out;
    size_t capacity;
    size_t used;
    bool overflowed;
} Sink;

static void put(Sink* sink, const char* format, ...) __attribute__((format(printf, 2, 3)));

static void put(Sink* sink, const char* format, ...) {
    if (sink->overflowed) {
        return;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(sink->out + sink->used, sink->capacity - sink->used, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= sink->capacity - sink->used) {
        sink->overflowed = true;
        return;
    }
    sink->used += (size_t)written;
}

static void put_bytes(Sink* sink, const char* data, size_t len) {
    if (sink->overflowed) {
        return;
    }
    if (len >= sink->capacity - sink->used) {
        sink->overflowed = true;
        return;
    }
    memcpy(sink->out + sink->used, data, len);
    sink->used += len;
    sink->out[sink->used] = '\0';
}

static void put_roster(Sink* sink, const World* world, const Room* room) {
    put(sink, "room=%s\nstatus=%s\nseats=%u\nhost=%u\n",
        room->code, status_name(room->status), (unsigned)room->seat_count, room->host.index);

    for (uint8_t i = 0; i < room->seat_count; i++) {
        const Player* const player = world_player(world, room->occupants[i]);
        put(sink, "seat=%u %u %s %d %d %s\n", (unsigned)i, room->occupants[i].index,
            player == NULL ? "(gone)" : player->name,
            room->seats[i].state.score_state.lines_cleared,
            room->seats[i].state.score_state.score,
            // "waiting" rather than "dead": a seat before START has no piece
            // yet, which is not the same thing as having lost
            room->status == ROOM_LOBBY ? "waiting" : (room->seats[i].alive ? "alive" : "dead"));
    }
}

int app_event_encode_room(const World* world, const Room* room,
                          char* out, size_t out_size, size_t* out_len) {
    Sink sink = {out, out_size, 0, false};
    put_roster(&sink, world, room);
    if (sink.overflowed) {
        return -1;
    }
    *out_len = sink.used;
    return 0;
}

/*
    The next queue is the tail of bag1 followed by bag2, which is how
    libtetrisbrain's two-bag generator orders upcoming pieces.
*/
static void put_next(Sink* sink, const BagState* bag) {
    put(sink, "next=");
    for (int k = 1; k < TETROMINO_TYPE_COUNT; k++) {
        const int pos = bag->bag1_offset + k;
        const TetrominoType type = pos < TETROMINO_TYPE_COUNT
                                       ? bag->bag1[pos]
                                       : bag->bag2[pos - TETROMINO_TYPE_COUNT];
        put(sink, "%c", TYPE_CHAR[type]);
    }
    put(sink, "\n");
}

int app_event_encode_state(const World* world, const Room* room, uint8_t seat,
                           char* out, size_t out_size, size_t* out_len) {
    Sink sink = {out, out_size, 0, false};
    const Seat* const s = &room->seats[seat];
    const State* const state = &s->state;

    put_roster(&sink, world, room);
    put(&sink, "frame=%llu\nyou=%u\n", (unsigned long long)room->frame, (unsigned)seat);

    const Tetromino* const piece = &state->board_state.tetromino;
    put(&sink, "piece=%c %d %d %d\n", TYPE_CHAR[piece->type], (int)piece->direction,
        piece->row_offset, piece->col_offset);
    put(&sink, "ghost=%d %d\n", state->board_state.ghost.ghost_row_offset,
        state->board_state.ghost.ghost_col_offset);

    if (state->hold_state.hold_status == HOLD_EMPTY) {
        put(&sink, "hold=-\n");
    }
    else {
        put(&sink, "hold=%c\n", TYPE_CHAR[state->hold_state.hold_type]);
    }
    put_next(&sink, &state->bag_state);

    put(&sink, "combo=%d\nb2b=%d\ngarbage=%d\nlast_clear=%d\n",
        state->combo_counter, state->back_to_back_count, state->garbage_balance,
        state->score_state.last_clear);

    // only the rows the client draws; the buffer rows above them exist so a
    // piece can spawn and rotate off-screen, and are nobody else's business
    put(&sink, "board=%d %d\n", BOARD_HEIGHT - VISIBLE_ROW_IDX, BOARD_WIDTH);
    for (int r = VISIBLE_ROW_IDX; r < BOARD_HEIGHT; r++) {
        char row[BOARD_WIDTH + 1];
        for (int c = 0; c < BOARD_WIDTH; c++) {
            row[c] = CELL_CHAR[state->board_state.board.cells[r][c]];
        }
        row[BOARD_WIDTH] = '\n';
        put_bytes(&sink, row, sizeof(row));
    }

    if (sink.overflowed) {
        return -1;
    }
    *out_len = sink.used;
    return 0;
}
