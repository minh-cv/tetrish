#ifndef TETRISH_TETRISU_STATE_CODEC_H
#define TETRISH_TETRISU_STATE_CODEC_H

#include "tetrisbrain.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAME_VIEW_ROWS (BOARD_HEIGHT - 20)
#define GAME_VIEW_COLS BOARD_WIDTH
#define GAME_VIEW_SEAT_MAX 8
#define GAME_VIEW_NAME_MAX 24
#define GAME_VIEW_CODE_MAX 16
#define GAME_VIEW_NEXT_MAX 8

typedef enum {
    GAME_STATUS_LOBBY,
    GAME_STATUS_RUNNING,
    GAME_STATUS_ENDED,
} GameStatus;

typedef struct {
    long id;
    char name[GAME_VIEW_NAME_MAX];
    int lines;
    int score;
    char status[8];   // waiting / alive / dead
} GameSeat;

/*!
    @brief The decoded form of one STATE push.

    A struct of its own rather than libtetrisbrain's State: the server sends
    what a renderer needs, not a simulation's internals, and inventing the
    missing halves of a State to fill in would invite someone to simulate from
    it. The client is not authoritative and this type is the reminder.
*/
typedef struct {
    char room[GAME_VIEW_CODE_MAX];
    GameStatus status;
    uint64_t frame;
    long host;

    GameSeat seats[GAME_VIEW_SEAT_MAX];
    size_t seat_count;
    size_t you;

    TetrominoType piece_type;
    TetrominoDirection piece_direction;
    int piece_row;
    int piece_col;
    int ghost_row;
    int ghost_col;

    TetrominoType hold;
    bool has_hold;
    TetrominoType next[GAME_VIEW_NEXT_MAX];
    size_t next_count;

    int combo;
    int back_to_back;
    int garbage;
    int last_clear;

    char cells[GAME_VIEW_ROWS][GAME_VIEW_COLS];
    bool has_board;
} GameView;

/*!
    @brief decode a STATE body into @p out

    Unknown keys are skipped rather than rejected, so a server that grows a
    field does not break an older client.

    @pre @p body is @p body_len bytes and need not be NUL-terminated
    @post on success every field named in the body is set and the rest keep
          their zeroed values
    @return -1 if the body is not a state snapshot at all
*/
int game_view_decode(GameView* out, const char* body, size_t body_len);

#endif
