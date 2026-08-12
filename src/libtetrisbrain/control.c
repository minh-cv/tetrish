#include "tetrisbrain/control.h"
#include "tetrisbrain/input.h"
#include "tetrisbrain/state.h"
#include "tetrisbrain/static.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum ApplyGravityResult {
    APPLIED,
    GROUNDED,
    WAITING,
} ApplyGravityResult;

static ApplyGravityResult apply_gravity_tick(const LockState* ls, GravityState* gs, BoardState* board_state, bool enable_soft_drop_this_frame) {
    const GhostPiece* ghost = &board_state->ghost;
    const Board* board = &board_state->board;
    Tetromino* t = &board_state->tetromino;

    if (ls->is_grounded && t->row_offset == ghost->ghost_row_offset) {
        return GROUNDED;
    }

    if (enable_soft_drop_this_frame) {
        gs->gravity_frame_counter = 0;

        int gravity = 1;
        if (gravity < gs->gravity) {
            gravity = gs->gravity;
        }

        for (int i = 0; i < gravity; i++) {
            if (!move_down(t, board)) {
                return GROUNDED;
            }
        }
        return APPLIED;
    }

    gs->gravity_frame_counter++;
    if (gs->gravity_frame_counter != gs->gravity_frame_counter_max) return WAITING;
    gs->gravity_frame_counter = 0;

    int gravity = gs->gravity;
    for (int i = 0; i < gravity; i++) {
        if (!move_down(t, board)) {
            gs->gravity_frame_counter = 0;
            return GROUNDED;
        }
    }
    return APPLIED;
}

static void init_new_piece(BoardState* bs, LockState* ls, GravityState* gs, MovementState* ms, TetrominoType spawn_type) {
    spawn(&bs->tetromino, spawn_type);
    ls->lock_counter = 0;
    ls->lock_movement_counter = 0;
    gs->gravity_frame_counter = 0;
    ls->is_grounded = false;
    ms->is_just_rotated = false;
    ms->last_offset_used = 0;
    
    update_ghost_piece(bs);
}

static void apply_movement(BoardState* s, LockState* ls, MovementState* ms, bool (*move_fn)(Tetromino *t, const Board *board, MovementState *ms)
) {
    if (!move_fn(&s->tetromino, &s->board, ms)) {
        return;
    }
    update_ghost_piece(s);
    if (ls->is_grounded && ls->lock_movement_counter < ls->lock_movement_counter_max) {
        ls->lock_movement_counter++;
        ls->lock_counter = 0;
    }
}

static bool apply_hold(State* s) {
    TetrominoType prev_type = s->hold_state.hold_type;
    HoldStatus prev_hold_status = s->hold_state.hold_status;
    hold(&s->hold_state, s->board_state.tetromino.type);
    if (prev_hold_status == HOLD_INACTIVE) {
        return false;
    }
    if (prev_hold_status == HOLD_EMPTY) {
        s->spawn_type = next_bag(&s->bag_state);
    }
    if (prev_hold_status == HOLD_ACTIVE) {
        s->spawn_type = prev_type;
    }
    return apply_spawn(s);
}

static bool apply_receive_garbage(State* s) {
    int received = s->garbage_balance;
    if (received > 0) {
        s->garbage_balance = 0;
        if (receive_garbage(&s->board_state, received)) return true;
    }

    s->spawn_type = next_bag(&s->bag_state);
    return apply_spawn(s);
}

static bool apply_unset_hold_inactive(State* s) {
    if (s->hold_state.hold_status == HOLD_INACTIVE) s->hold_state.hold_status = HOLD_ACTIVE;
    return apply_receive_garbage(s);
}

static bool apply_store_garbage(State* s, int lines_cleared, TSpinType t_spin) {
    if (lines_cleared >= 1) {
        int counter_amount = get_sending_garbage(s, lines_cleared, t_spin);
        s->garbage_balance -= counter_amount;
    }
    return apply_unset_hold_inactive(s);
}

void apply_attack(State* src, State* dest) {
    if (src->garbage_balance < 0) {
        queue_garbage(dest, -src->garbage_balance);
        src->garbage_balance = 0;
    }
}

static bool apply_lock_and_clear(State* s) {
    TSpinType t_spin = get_t_spin_type(&s->board_state.tetromino, s->movement_state.last_offset_used, s->movement_state.is_just_rotated, &s->board_state.board);

    BoardState* bs = &s->board_state;
    int lines_cleared = lock_and_clear(&bs->tetromino, &bs->board);
    if (lines_cleared > 0) {
        s->combo_counter++;
        s->back_to_back_count = (lines_cleared == 4 || t_spin != T_SPIN_NONE) ? s->back_to_back_count + 1 : -1;
    }
    else {
        s->combo_counter = -1;
    }
    
    return apply_store_garbage(s, lines_cleared, t_spin);
}


static bool apply_frame(State* s, bool enable_soft_drop) {
    BoardState* board_state = &s->board_state; 
    LockState* lock_state = &s->lock_state;
    GravityState* gravity_state = &s->gravity_state;

    int prev_row = board_state->tetromino.row_offset;
    ApplyGravityResult result = apply_gravity_tick(lock_state, gravity_state, board_state, enable_soft_drop);
    int current_row = board_state->tetromino.row_offset;
    if (current_row != prev_row) {
        lock_state->lock_counter = 0;
        s->movement_state.is_just_rotated = false;
    }
    lock_state->is_grounded = result == GROUNDED;
    if (!lock_state->is_grounded) {
        return false;
    }
    // Piece is on the ground — start or continue lock delay
    lock_state->lock_counter++;
    if (lock_state->lock_counter < lock_state->lock_counter_max) {
        return false;
    }

    return apply_lock_and_clear(s);
}

bool apply_spawn(State* s) {
    init_new_piece(&s->board_state, &s->lock_state, &s->gravity_state, &s->movement_state, s->spawn_type);
    if (is_colliding(&s->board_state.tetromino, &s->board_state.board)) {
        return true;
    }
    return false;
}

bool apply_player_inputs(State* s, const bool (*inputs)[PLAYER_INPUT_KEY_COUNT]) {
    if ((*inputs)[PLAYER_INPUT_KEY_HOLD] && s->hold_state.hold_status != HOLD_INACTIVE) {
        return apply_hold(s);
    }

    if ((*inputs)[PLAYER_INPUT_KEY_LOCK_DOWN]) {
        if (s->board_state.tetromino.row_offset != s->board_state.ghost.ghost_row_offset) {
            s->board_state.tetromino.row_offset = s->board_state.ghost.ghost_row_offset;
            s->movement_state.is_just_rotated = false;
        }
        return apply_lock_and_clear(s);
    }

    if ((*inputs)[PLAYER_INPUT_KEY_MOVE_LEFT])
        apply_movement(&s->board_state, &s->lock_state, &s->movement_state, move_left);

    if ((*inputs)[PLAYER_INPUT_KEY_MOVE_RIGHT])
        apply_movement(&s->board_state, &s->lock_state, &s->movement_state, move_right);

    if ((*inputs)[PLAYER_INPUT_KEY_ROTATE_LEFT])
        apply_movement(&s->board_state, &s->lock_state, &s->movement_state, rotate_left);

    if ((*inputs)[PLAYER_INPUT_KEY_ROTATE_RIGHT])
        apply_movement(&s->board_state, &s->lock_state, &s->movement_state, rotate_right);

    return apply_frame(s, (*inputs)[PLAYER_INPUT_KEY_SOFT_DROP]);
}