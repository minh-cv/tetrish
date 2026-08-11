#include "state_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    A cursor over the body. Lines are handed out one at a time as
    NUL-terminated copies in a scratch buffer, which keeps the field parsers
    free to use sscanf without ever needing the body itself to be terminated.
*/
typedef struct {
    const char* p;
    const char* end;
} Cursor;

#define LINE_MAX 128

static bool next_line(Cursor* cursor, char* out, size_t out_size) {
    if (cursor->p >= cursor->end) {
        return false;
    }
    const char* q = cursor->p;
    while (q < cursor->end && *q != '\n') {
        q++;
    }

    size_t len = (size_t)(q - cursor->p);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, cursor->p, len);
    out[len] = '\0';

    cursor->p = q < cursor->end ? q + 1 : cursor->end;
    return true;
}

static bool starts_with(const char* line, const char* key, const char** rest) {
    const size_t len = strlen(key);
    if (strncmp(line, key, len) != 0) {
        return false;
    }
    *rest = line + len;
    return true;
}

static TetrominoType type_of(char c) {
    switch (c) {
    case 'I': return TETROMINO_TYPE_I;
    case 'J': return TETROMINO_TYPE_J;
    case 'L': return TETROMINO_TYPE_L;
    case 'O': return TETROMINO_TYPE_O;
    case 'S': return TETROMINO_TYPE_S;
    case 'T': return TETROMINO_TYPE_T;
    case 'Z': return TETROMINO_TYPE_Z;
    default: return TETROMINO_TYPE_I;
    }
}

static GameStatus status_of(const char* text) {
    if (strcmp(text, "running") == 0) {
        return GAME_STATUS_RUNNING;
    }
    if (strcmp(text, "ended") == 0) {
        return GAME_STATUS_ENDED;
    }
    return GAME_STATUS_LOBBY;
}

static void parse_seat(GameView* out, const char* rest) {
    unsigned index;
    long id;
    char name[GAME_VIEW_NAME_MAX];
    int lines;
    int score;
    char status[8];

    if (sscanf(rest, "%u %ld %23s %d %d %7s", &index, &id, name, &lines, &score, status) != 6) {
        return;
    }
    if (index >= GAME_VIEW_SEAT_MAX) {
        return;
    }

    GameSeat* const seat = &out->seats[index];
    seat->id = id;
    snprintf(seat->name, sizeof(seat->name), "%s", name);
    seat->lines = lines;
    seat->score = score;
    snprintf(seat->status, sizeof(seat->status), "%s", status);
}

int game_view_decode(GameView* out, const char* body, size_t body_len) {
    memset(out, 0, sizeof(*out));

    Cursor cursor = {body, body + body_len};
    char line[LINE_MAX];
    size_t board_rows = 0;
    size_t board_row_index = 0;
    bool saw_room = false;

    while (next_line(&cursor, line, sizeof(line))) {
        const char* rest;

        // the board block is the only multi-line field, so the rows are
        // consumed by this branch rather than by the key dispatch below
        if (board_rows != 0 && board_row_index < board_rows) {
            if (board_row_index < GAME_VIEW_ROWS) {
                for (size_t c = 0; c < GAME_VIEW_COLS && line[c] != '\0'; c++) {
                    out->cells[board_row_index][c] = line[c];
                }
            }
            board_row_index++;
            if (board_row_index == board_rows) {
                out->has_board = true;
            }
            continue;
        }

        if (starts_with(line, "room=", &rest)) {
            snprintf(out->room, sizeof(out->room), "%s", rest);
            saw_room = true;
        }
        else if (starts_with(line, "status=", &rest)) {
            out->status = status_of(rest);
        }
        else if (starts_with(line, "seats=", &rest)) {
            const long count = strtol(rest, NULL, 10);
            out->seat_count = count > 0 && count <= GAME_VIEW_SEAT_MAX ? (size_t)count : 0;
        }
        else if (starts_with(line, "host=", &rest)) {
            out->host = strtol(rest, NULL, 10);
        }
        else if (starts_with(line, "seat=", &rest)) {
            parse_seat(out, rest);
        }
        else if (starts_with(line, "frame=", &rest)) {
            out->frame = strtoull(rest, NULL, 10);
        }
        else if (starts_with(line, "you=", &rest)) {
            const long you = strtol(rest, NULL, 10);
            out->you = you > 0 && you < GAME_VIEW_SEAT_MAX ? (size_t)you : 0;
        }
        else if (starts_with(line, "piece=", &rest)) {
            char type;
            int direction;
            int row;
            int col;
            if (sscanf(rest, "%c %d %d %d", &type, &direction, &row, &col) == 4) {
                out->piece_type = type_of(type);
                out->piece_direction = (TetrominoDirection)direction;
                out->piece_row = row;
                out->piece_col = col;
            }
        }
        else if (starts_with(line, "ghost=", &rest)) {
            sscanf(rest, "%d %d", &out->ghost_row, &out->ghost_col);
        }
        else if (starts_with(line, "hold=", &rest)) {
            out->has_hold = rest[0] != '-';
            if (out->has_hold) {
                out->hold = type_of(rest[0]);
            }
        }
        else if (starts_with(line, "next=", &rest)) {
            out->next_count = 0;
            for (size_t i = 0; rest[i] != '\0' && out->next_count < GAME_VIEW_NEXT_MAX; i++) {
                out->next[out->next_count++] = type_of(rest[i]);
            }
        }
        else if (starts_with(line, "combo=", &rest)) {
            out->combo = (int)strtol(rest, NULL, 10);
        }
        else if (starts_with(line, "b2b=", &rest)) {
            out->back_to_back = (int)strtol(rest, NULL, 10);
        }
        else if (starts_with(line, "garbage=", &rest)) {
            out->garbage = (int)strtol(rest, NULL, 10);
        }
        else if (starts_with(line, "last_clear=", &rest)) {
            out->last_clear = (int)strtol(rest, NULL, 10);
        }
        else if (starts_with(line, "board=", &rest)) {
            const long rows = strtol(rest, NULL, 10);
            board_rows = rows > 0 ? (size_t)rows : 0;
            board_row_index = 0;
        }
    }

    return saw_room ? 0 : -1;
}
