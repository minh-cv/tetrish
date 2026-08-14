#include "tetrisbrain/state.h"
#include "tetrisbrain/static.h"

#include <assert.h>
#include <string.h>

static void fill_random_bag(TetrominoType bag[TETROMINO_TYPE_COUNT], Rng* rng);

StateConfig state_config_default(void) {
    const StateConfig config = {
        .start_level = 0,
        .frames_per_level_up = 0,
        .lock_counter_max = 30,
        .lock_movement_counter_max = 15,
    };
    return config;
}

State init_state(uint64_t seed, const StateConfig* config) {
    const StateConfig default_config = state_config_default();
    if (config == NULL) {
        config = &default_config;
    }
    State s = {
        .lock_state = {
            0,
            0,
            config->lock_counter_max,
            config->lock_movement_counter_max,
            false,
        },
        .level_state = {
            config->start_level,
            0,
            config->frames_per_level_up,
        },
    };
    update_level_gravity(&s.gravity_state, config->start_level);
    for (int r = 0; r < BOARD_HEIGHT; r++) {
        for (int c = 0; c < BOARD_WIDTH; c++) {
            s.board_state.board.cells[r][c] = TETROMINO_CELL_EMPTY;
        }
    }
    rng_init(&s.rng, seed);
    s.bag_state.bag1_offset = -1;
    fill_random_bag(s.bag_state.bag1, &s.rng);
    fill_random_bag(s.bag_state.bag2, &s.rng);
    s.spawn_type = next_bag(&s.bag_state, &s.rng);
    s.combo_counter = -1;
    s.back_to_back_count = -1;

    update_ghost_piece(&s.board_state);
    return s;
}

/*
    One place owns the level -> speed curve: the fall period shrinks by two
    frames per level down to one frame, and past that point the piece starts
    falling multiple cells per step instead.
*/
void update_level_gravity(GravityState* gs, int level) {
    int counter_max = 30 - 2 * level;
    if (counter_max < 1) {
        counter_max = 1;
    }
    gs->gravity_frame_counter_max = counter_max;
    gs->gravity = level > 14 ? 1 + (level - 14) : 1;
}

static void fill_random_bag(TetrominoType bag[TETROMINO_TYPE_COUNT], Rng* rng) {
    for (int i = 0; i < TETROMINO_TYPE_COUNT; i++) {
        bag[i] = (TetrominoType)i;
    }

    for (int i = TETROMINO_TYPE_COUNT - 1; i > 0; i--) {
        int j = rng_below(rng, i + 1);
        TetrominoType tmp = bag[i];
        bag[i] = bag[j];
        bag[j] = tmp;
    }
}

TetrominoType next_bag(BagState* s, Rng* rng) {
    s->bag1_offset++;
    if (s->bag1_offset == TETROMINO_TYPE_COUNT) {
        s->bag1_offset = 0;
        memcpy(s->bag1, s->bag2, sizeof(s->bag1));
        fill_random_bag(s->bag2, rng);
    }
    return s->bag1[s->bag1_offset];
}

void spawn(Tetromino* t, TetrominoType type) {
    t->type = type;
    t->direction = TETROMINO_DIRECTION_0;
    t->row_offset = TETROMINO_STARTING_ROW[t->type];
    t->col_offset = TETROMINO_STARTING_COL[t->type];
}

bool is_colliding(const Tetromino* t, const Board* board) {
    for (int i = 0; i < TETROMINO_BOX_SIZE[t->type]; i++) {
        for (int j = 0; j < TETROMINO_BOX_SIZE[t->type]; j++) {
            if (IS_TETROMINO_CELL_OCCUPIED[t->type](t->type, t->direction, i, j) &&
                (t->row_offset + i < 0 || t->row_offset + i >= BOARD_HEIGHT ||
                t->col_offset + j < 0 || t->col_offset + j >= BOARD_WIDTH ||
                (board->cells)[t->row_offset + i][t->col_offset + j] != TETROMINO_CELL_EMPTY)) {
                return true;
            }
        }
    }
    return false;
}

bool move_down(Tetromino* t, const Board* board) {
    t->row_offset++;
    if (!is_colliding(t, board)) {
        return true;
    }
    t->row_offset--;
    return false;
}

bool move_left(Tetromino* t, const Board* board, MovementState* ms) {
    t->col_offset--;
    if (!is_colliding(t, board)) {
        ms->is_just_rotated = false;
        return true;
    }
    t->col_offset++;
    return false;
}

bool move_right(Tetromino* t, const Board* board, MovementState* ms) {
    t->col_offset++;
    if (!is_colliding(t, board)) {
        ms->is_just_rotated = false;
        return true;
    }
    t->col_offset--;
    return false;
}

bool rotate(Tetromino* t, const Board* board, MovementState* ms, TetrominoDirection new_direction) {   
    TetrominoDirection current_dir = t->direction;
    t->direction = new_direction;

    int og_col_offset = t->col_offset;
    int og_row_offset = t->row_offset;

    for (int i = 0; i < 5; i++) {
        int current_col_offset = TETROMINO_OFFSET[t->type][current_dir][i][0];
        int current_row_offset = TETROMINO_OFFSET[t->type][current_dir][i][1];
        int new_col_offset = TETROMINO_OFFSET[t->type][t->direction][i][0];
        int new_row_offset = TETROMINO_OFFSET[t->type][t->direction][i][1];

        t->col_offset = og_col_offset + current_col_offset - new_col_offset;
        t->row_offset = og_row_offset - current_row_offset + new_row_offset;

        if (!is_colliding(t, board)) {
            ms->is_just_rotated = true;
            ms->last_offset_used = i;
            return true;
        }
    }
    t->col_offset = og_col_offset;
    t->row_offset = og_row_offset;
    t->direction = current_dir;
    return false;
}

bool rotate_right(Tetromino* t, const Board* board, MovementState* ms) {
    return rotate(t, board, ms, TETROMINO_RIGHT_DIRECTION[t->direction]);
}

bool rotate_left(Tetromino* t, const Board* board, MovementState* ms) {
    return rotate(t, board, ms, TETROMINO_LEFT_DIRECTION[t->direction]);
}

void lock_tetromino(const Tetromino* t, Board* board) {
    for (int i = 0; i < TETROMINO_BOX_SIZE[t->type]; i++) {
        for (int j = 0; j < TETROMINO_BOX_SIZE[t->type]; j++) {
            if (IS_TETROMINO_CELL_OCCUPIED[t->type](t->type, t->direction, i, j)) {
                (board->cells)[t->row_offset + i][t->col_offset + j] = (TetrominoCellType)t->type;
            }
        }
    }
}

int clear_line(const Tetromino* t, Board* board) {
    int cleared = 0;
    int write_row = t->row_offset + TETROMINO_BOX_SIZE[t->type] - 1;
    if (write_row >= BOARD_HEIGHT) {
        write_row = BOARD_HEIGHT - 1;
    }
    
    const int min_read_row = t->row_offset < 0 ? 0 : t->row_offset;
    for (int read_row = write_row; read_row >= min_read_row; read_row--) {
        bool full = true;
        for (int col = 0; col < BOARD_WIDTH; col++) {
            if ((board->cells)[read_row][col] == TETROMINO_CELL_EMPTY) {
                full = false;
                break;
            }
        }

        if (full) {
            cleared++;
            continue;
        }

        if (read_row != write_row) {
            for (int i = 0; i < BOARD_WIDTH; i++) {
                (board->cells)[write_row][i] = (board->cells)[read_row][i];
            }
        }
        write_row--;
    }

    for (int read_row = t->row_offset - 1; read_row >= 0; read_row--) {
        if (read_row != write_row) {
            for (int i = 0; i < BOARD_WIDTH; i++) {
                (board->cells)[write_row][i] = (board->cells)[read_row][i];
            }
        }
        write_row--;
    }

    for (int row = write_row; row >= 0; row--) {
        for (int col = 0; col < BOARD_WIDTH; col++) {
            (board->cells)[row][col] = TETROMINO_CELL_EMPTY;
        }
    }

    return cleared;
}

int lock_and_clear(const Tetromino* t, Board* board) {
    lock_tetromino(t, board);
    return clear_line(t, board);
}

TSpinType get_t_spin_type(const Tetromino* t, int last_offset_used, bool is_just_rotated, const Board* board) {
    if (!(t->type == TETROMINO_TYPE_T && 
        is_just_rotated)) {
        return T_SPIN_NONE;
    }

    static const int CORNERS[4][2] = {
        {0, 0}, {0, 2}, {2, 2}, {2, 0}
    };

    // corresponding back-corner is (2 - CORNERS[dir][0], 2 - CORNERS[dir][1])
    int front_corner_count = 0;
    int back_corner_count = 0;

    int row_cell = t->row_offset + CORNERS[t->direction][0];
    int col_cell = t->col_offset + CORNERS[t->direction][1];
    
    if (row_cell < 0 || row_cell >= BOARD_HEIGHT ||
    col_cell < 0 || col_cell >= BOARD_WIDTH ||
    (board->cells)[row_cell][col_cell] != TETROMINO_CELL_EMPTY) {
        front_corner_count++;
    }

    row_cell = t->row_offset + 2 - CORNERS[t->direction][0];
    col_cell = t->col_offset + 2 - CORNERS[t->direction][1];

    if (row_cell < 0 || row_cell >= BOARD_HEIGHT ||
    col_cell < 0 || col_cell >= BOARD_WIDTH ||
    (board->cells)[row_cell][col_cell] != TETROMINO_CELL_EMPTY) {
        back_corner_count++;
    }

    TetrominoDirection other_dir = TETROMINO_RIGHT_DIRECTION[t->direction];
    row_cell = t->row_offset + CORNERS[other_dir][0];
    col_cell = t->col_offset + CORNERS[other_dir][1];
    
    if (row_cell < 0 || row_cell >= BOARD_HEIGHT ||
    col_cell < 0 || col_cell >= BOARD_WIDTH ||
    (board->cells)[row_cell][col_cell] != TETROMINO_CELL_EMPTY) {
        front_corner_count++;
    }

    row_cell = t->row_offset + 2 - CORNERS[other_dir][0];
    col_cell = t->col_offset + 2 - CORNERS[other_dir][1];

    if (row_cell < 0 || row_cell >= BOARD_HEIGHT ||
    col_cell < 0 || col_cell >= BOARD_WIDTH ||
    (board->cells)[row_cell][col_cell] != TETROMINO_CELL_EMPTY) {
        back_corner_count++;
    }

    if (back_corner_count + front_corner_count < 3) {
        return T_SPIN_NONE;
    }

    if (front_corner_count == 2 || last_offset_used == 4) {
        return T_SPIN_PROPER;
    }
    return T_SPIN_MINI;
}

void update_ghost_piece(BoardState* bs) {
    GhostPiece* ghost = &bs->ghost;
    const Tetromino* ref_t = &bs->tetromino;
    const Board* board = &bs->board;

    ghost->ghost_col_offset = ref_t->col_offset;
    ghost->ghost_row_offset = ref_t->row_offset;
    for (; ghost->ghost_row_offset <= BOARD_HEIGHT; ghost->ghost_row_offset++) {
        const Tetromino ghost_tetromino = {
            ref_t->type,
            ref_t->direction,
            ghost->ghost_col_offset,
            ghost->ghost_row_offset,
        };
        if (is_colliding(&ghost_tetromino, board)) {
            ghost->ghost_row_offset--;
            return;
        }
    }
    assert(false);
}

void add_garbage_lines(Board* board, int line_count, Rng* rng) {
    if (line_count <= 0) return;
    if (line_count > BOARD_HEIGHT) line_count = BOARD_HEIGHT;

    for (int i = line_count; i < BOARD_HEIGHT; i++) {
        for (int j = 0; j < BOARD_WIDTH; j++) {
            (board->cells)[i - line_count][j] = (board->cells)[i][j];
        }
    }
    int empty_col = rng_below(rng, BOARD_WIDTH);
    for (int i = BOARD_HEIGHT - line_count; i < BOARD_HEIGHT; i++) {
        for (int j = 0; j < BOARD_WIDTH; j++) {
            if (j != empty_col) {
                (board->cells)[i][j] = TETROMINO_CELL_GARBAGE;
                continue;
            }
            (board->cells)[i][j] = TETROMINO_CELL_EMPTY;
        }
    }
}

bool is_outside_playfield_area(Tetromino* t) {
    for (int i = 0; i < TETROMINO_BOX_SIZE[t->type]; i++) {
        for (int j = 0; j < TETROMINO_BOX_SIZE[t->type]; j++) {
            if (IS_TETROMINO_CELL_OCCUPIED[t->type](t->type, t->direction, i, j)) {
                return t->row_offset + i < 0;
            }
        }
    }
    assert(false && "empty piece");
    return false;
}

bool push_tetromino(Tetromino* t, const Board* board) {
    while (is_colliding(t, board)) {
        t->row_offset--;
        if (is_outside_playfield_area(t)) return true;
    }
    return false;
}

bool receive_garbage(BoardState* s, int count, Rng* rng) {
    add_garbage_lines(&s->board, count, rng);
    bool is_game_over = push_tetromino(&s->tetromino, &s->board);
    if (is_game_over) return true;
    update_ghost_piece(s);
    return false;
}

void hold(HoldState* hold_state, TetrominoType current_type) {
    if (hold_state->hold_status == HOLD_INACTIVE) return;
    hold_state->hold_type = current_type;
    hold_state->hold_status = HOLD_INACTIVE;
}

void queue_garbage(State* s, int amount) {
    s->garbage_balance += amount;
}

static bool is_perfect_clear(const Board* s) {
    for (int i = 0; i < BOARD_HEIGHT; i++) {
        for (int j = 0; j < BOARD_WIDTH; j++) {
            if ((s->cells)[i][j] != TETROMINO_CELL_EMPTY) return false;
        }
    }
    return true;
}

void add_score(State* s, int garbage_amount) {
    assert(garbage_amount >= 0);

    // Widen first, then saturate — a long game must never wrap the score negative.
    long long total = (long long)s->score + (long long)garbage_amount * SCORE_PER_GARBAGE;
    s->score = total > SCORE_MAX ? SCORE_MAX : (int)total;
}

int get_sending_garbage(const State* s, int lines_cleared, TSpinType t_spin) {
    static const int COMBO_AMOUNTS[] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
    static const int T_SPIN_AMOUNTS[] = {0, 2, 4, 6};

    int counter_amount = 0;
    switch (t_spin) {
        case T_SPIN_PROPER:
            assert(lines_cleared <= 3 && "T-spin cannot clear more than 3 lines");
            counter_amount = T_SPIN_AMOUNTS[lines_cleared];
            break;
        default:
            if (lines_cleared > 1) {
                counter_amount = 1 << (lines_cleared - 2);
            }
            break;
    }

    if (is_perfect_clear(&s->board_state.board)) counter_amount += 10;
    if (s->back_to_back_count > 0) { counter_amount += 1; } // or other constants or with scaling

    if (s->combo_counter > 0) {
        int combo_amount = 5;
        if ((size_t)s->combo_counter < sizeof(COMBO_AMOUNTS)/sizeof(int)) {
            combo_amount = COMBO_AMOUNTS[s->combo_counter];
        }
        counter_amount += combo_amount;
    }

    return counter_amount;
}