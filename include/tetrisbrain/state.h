#ifndef TETRISH_BRAIN_STATE_H
#define TETRISH_BRAIN_STATE_H

#include "static.h"

typedef struct Board {
    TetrominoCellType cells[BOARD_HEIGHT][BOARD_WIDTH];
} Board;

typedef struct Tetromino {
    TetrominoType type;
    TetrominoDirection direction;
    int col_offset;
    int row_offset;
} Tetromino;

typedef struct GhostPiece {
    int ghost_col_offset;
    int ghost_row_offset;
} GhostPiece;

typedef enum HoldStatus {
    HOLD_EMPTY,
    HOLD_INACTIVE,
    HOLD_ACTIVE,
} HoldStatus;

typedef struct LockState {
    int lock_counter;
    int lock_movement_counter;
    int lock_counter_max;
    int lock_movement_counter_max;
    bool is_grounded;
} LockState;

typedef struct BagState {
    TetrominoType bag1[TETROMINO_TYPE_COUNT];
    TetrominoType bag2[TETROMINO_TYPE_COUNT];
    int bag1_offset;
} BagState;

typedef struct GravityState {
    int gravity;
    int gravity_frame_counter;
    int gravity_frame_counter_max;
} GravityState;

typedef struct HoldState {
    TetrominoType hold_type;
    HoldStatus hold_status;
} HoldState;

typedef struct BoardState {
    Board board;
    Tetromino tetromino;
    GhostPiece ghost;
} BoardState;

/*!
    @brief The line-clear and score tracker the spec asks to be visible.

    @c last_clear is the size of the most recent clear rather than a running
    total, so a renderer can announce "TETRIS" without keeping its own history.
*/
typedef struct ScoreState {
    int lines_cleared;
    int score;
    int last_clear;
} ScoreState;

typedef struct MovementState {
    int last_offset_used;
    bool is_just_rotated;
} MovementState;

typedef struct State {
    BoardState board_state; 
    LockState lock_state;
    GravityState gravity_state;
    int combo_counter;
    HoldState hold_state;
    BagState bag_state;
    TetrominoType spawn_type;
    MovementState movement_state;
    int garbage_balance;
    int back_to_back_count;
    ScoreState score_state;
} State;

typedef enum TSpinType {
    T_SPIN_NONE,
    T_SPIN_PROPER,
    T_SPIN_MINI,
} TSpinType;

State init_state(void);
TetrominoType next_bag(BagState* s);
void spawn(Tetromino* t, TetrominoType type);
bool is_colliding(const Tetromino* t, const Board* board);
bool move_down(Tetromino* t, const Board* board);
bool move_left(Tetromino* t, const Board* board, MovementState* ms);
bool move_right(Tetromino* t, const Board* board, MovementState* ms);
bool rotate(Tetromino* t, const Board* board, MovementState* ms, TetrominoDirection new_direction);
bool rotate_right(Tetromino* t, const Board* board, MovementState* ms);
bool rotate_left(Tetromino* t, const Board* board, MovementState* ms);
void lock_tetromino(const Tetromino* t, Board* board);
int clear_line(const Tetromino* t, Board* board);
int lock_and_clear(const Tetromino* t, Board* board);
void update_ghost_piece(BoardState* bs);
void add_garbage_lines(Board* board, int line_count);
bool is_outside_playfield_area(Tetromino* t);
bool push_tetromino(Tetromino* t, const Board* board);
bool receive_garbage(BoardState* s, int count);
void hold(HoldState* hold_state, TetrominoType current_type);
void queue_garbage(State* s, int amount);
int get_sending_garbage(const State* s, int lines_cleared, TSpinType t_spin);
TSpinType get_t_spin_type(const Tetromino* t, int last_offset_used, bool is_just_rotated, const Board* board);

#endif