#include "proto.h"

#include "tetrisbrain/state.h"
#include "tetrisbrain/static.h"
#include "wire.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
    The wire is two's-complement big-endian int32, so a host int has to be
    exactly that wide for the round trip to be lossless: a wider one holds
    values the encoder would truncate, a narrower one cannot hold every value
    the decoder can read. Stated as the range rather than as sizeof, since
    that is the property actually needed and sizeof also answers to CHAR_BIT
    and padding bits.
*/
_Static_assert(INT_MAX == INT32_MAX && INT_MIN == INT32_MIN, "proto encodes int as a 4-byte field");

/*
    Pins the layout table in proto.h. Adding a field to ProtoStateRequest fires
    this, and the table has to be updated with it.
*/
_Static_assert(PROTO_STATE_REQUEST_BODY_LEN == 452, "proto.h layout table is stale");

//! @brief One past the last HoldStatus enumerator; the enum has no count member.
#define HOLD_STATUS_COUNT (HOLD_ACTIVE + 1)

static const char* const MOVE_TOKEN[] = {
    [PROTO_MOVE_LEFT] = "LEFT",
    [PROTO_MOVE_RIGHT] = "RIGHT",
};

static const char* const ROTATE_TOKEN[] = {
    [PROTO_ROTATE_CW] = "CW",
    [PROTO_ROTATE_CCW] = "CCW",
};

static const char* const DROP_TOKEN[] = {
    [PROTO_DROP_SOFT] = "SOFT",
    [PROTO_DROP_HARD] = "HARD",
};

/*!
    @brief Index of the entry of @p token that @p body spells exactly.

    @return -1 when no entry matches, the index otherwise.
*/
static int parse_token(const unsigned char* body, size_t body_len, const char* const* token, size_t count) {
    if (body == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const size_t len = strlen(token[i]);
        if (len == body_len && memcmp(body, token[i], len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*!
    @brief Hand out the entry of @p token at @p value as a static body.

    @pre @p value is a valid index; out of range falls back to `0` after the
         assert, per the header's release-build contract. A negative
         enumerator wraps to a large @c size_t and is caught by the same
         check.
*/
static const unsigned char* serialize_token(size_t value, const char* const* token, size_t count, size_t* body_len) {
    assert(value < count && "enumerator out of range");
    if (value >= count) {
        value = 0;
    }
    *body_len = strlen(token[value]);
    return (const unsigned char*)token[value];
}

static void put_u8(unsigned char* body, size_t* off, unsigned char value) {
    body[*off] = value;
    *off += 1;
}

/*!
    @brief Write @p value as one byte after checking it names an enumerator.

    @pre `0 <= value < limit`; out of range is asserted and then truncated,
         per the header's release-build contract.
*/
static void put_enum(unsigned char* body, size_t* off, int value, int limit) {
    assert(value >= 0 && value < limit && "enumerator out of range");
    (void)limit;
    put_u8(body, off, (unsigned char)value);
}

static void put_i32(unsigned char* body, size_t* off, int value) {
    uint8_t tmp[4];
    encode_u32_be(tmp, (uint32_t)value);
    memcpy(body + *off, tmp, sizeof(tmp));
    *off += sizeof(tmp);
}

static unsigned char get_u8(const unsigned char* body, size_t* off) {
    const unsigned char value = body[*off];
    *off += 1;
    return value;
}

/*!
    @brief Read one byte and check it names an enumerator of `[0, limit)`.

    @post @p out is written only when the byte is in range.

    @return -1 when it is not, 0 otherwise.
*/
static int get_enum(const unsigned char* body, size_t* off, int limit, int* out) {
    const unsigned char value = get_u8(body, off);
    if ((int)value >= limit) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int get_i32(const unsigned char* body, size_t* off) {
    uint8_t tmp[4];
    memcpy(tmp, body + *off, sizeof(tmp));
    *off += sizeof(tmp);

    const uint32_t value = decode_u32_be(tmp);
    if (value <= (uint32_t)INT32_MAX) {
        return (int)value;
    }
    /*
        Converting an out-of-range uint32_t straight back to int32_t is
        implementation-defined, so fold it into range first: the subtraction
        lands in [0, INT32_MAX] and the addition cannot overflow.
    */
    return (int)(value - (uint32_t)INT32_MIN) + INT32_MIN;
}

int proto_parse_state_request(const unsigned char* body, size_t body_len, ProtoStateRequest* state) {
    if (body == NULL || body_len != PROTO_STATE_REQUEST_BODY_LEN) {
        return -1;
    }

    /*
        Decoded into a local so that a byte failing validation halfway
        through leaves the caller's struct untouched.
    */
    ProtoStateRequest out;
    memset(&out, 0, sizeof(out));
    size_t off = 0;

    for (int row = 0; row < BOARD_HEIGHT; row++) {
        for (int col = 0; col < BOARD_WIDTH; col++) {
            int cell;
            if (get_enum(body, &off, TETROMINO_CELL_COUNT, &cell) == -1) {
                return -1;
            }
            out.board_state.board.cells[row][col] = (TetrominoCellType)cell;
        }
    }

    int type;
    int direction;
    if (get_enum(body, &off, TETROMINO_TYPE_COUNT, &type) == -1 ||
        get_enum(body, &off, TETROMINO_DIRECTION_COUNT, &direction) == -1) {
        return -1;
    }
    out.board_state.tetromino.type = (TetrominoType)type;
    out.board_state.tetromino.direction = (TetrominoDirection)direction;
    out.board_state.tetromino.col_offset = get_i32(body, &off);
    out.board_state.tetromino.row_offset = get_i32(body, &off);
    out.board_state.ghost.ghost_col_offset = get_i32(body, &off);
    out.board_state.ghost.ghost_row_offset = get_i32(body, &off);
    out.combo_counter = get_i32(body, &off);

    int hold_type;
    int hold_status;
    if (get_enum(body, &off, TETROMINO_TYPE_COUNT, &hold_type) == -1 ||
        get_enum(body, &off, HOLD_STATUS_COUNT, &hold_status) == -1) {
        return -1;
    }
    out.hold_state.hold_type = (TetrominoType)hold_type;
    out.hold_state.hold_status = (HoldStatus)hold_status;

    for (int i = 0; i < TETROMINO_TYPE_COUNT; i++) {
        int bag;
        if (get_enum(body, &off, TETROMINO_TYPE_COUNT, &bag) == -1) {
            return -1;
        }
        out.bag_state.bag1[i] = (TetrominoType)bag;
    }
    for (int i = 0; i < TETROMINO_TYPE_COUNT; i++) {
        int bag;
        if (get_enum(body, &off, TETROMINO_TYPE_COUNT, &bag) == -1) {
            return -1;
        }
        out.bag_state.bag2[i] = (TetrominoType)bag;
    }

    /*
        next_bag wraps the offset at TETROMINO_TYPE_COUNT, so it indexes bag1
        rather than pointing one past it.
    */
    if (get_enum(body, &off, TETROMINO_TYPE_COUNT, &out.bag_state.bag1_offset) == -1) {
        return -1;
    }

    out.garbage_balance = get_i32(body, &off);
    out.back_to_back_count = get_i32(body, &off);
    out.game_score = get_i32(body, &off);

    const unsigned char is_game_active = get_u8(body, &off);
    if (is_game_active > 1) {
        return -1;
    }
    out.is_game_active = is_game_active == 1;

    assert(off == PROTO_STATE_REQUEST_BODY_LEN && "decoder disagrees with the layout table");

    *state = out;
    return 0;
}

void proto_encode_state_request(const ProtoStateRequest* state, unsigned char body[PROTO_STATE_REQUEST_BODY_LEN]) {
    size_t off = 0;

    for (int row = 0; row < BOARD_HEIGHT; row++) {
        for (int col = 0; col < BOARD_WIDTH; col++) {
            put_enum(body, &off, (int)state->board_state.board.cells[row][col], TETROMINO_CELL_COUNT);
        }
    }

    put_enum(body, &off, (int)state->board_state.tetromino.type, TETROMINO_TYPE_COUNT);
    put_enum(body, &off, (int)state->board_state.tetromino.direction, TETROMINO_DIRECTION_COUNT);
    put_i32(body, &off, state->board_state.tetromino.col_offset);
    put_i32(body, &off, state->board_state.tetromino.row_offset);
    put_i32(body, &off, state->board_state.ghost.ghost_col_offset);
    put_i32(body, &off, state->board_state.ghost.ghost_row_offset);
    put_i32(body, &off, state->combo_counter);

    put_enum(body, &off, (int)state->hold_state.hold_type, TETROMINO_TYPE_COUNT);
    put_enum(body, &off, (int)state->hold_state.hold_status, HOLD_STATUS_COUNT);

    for (int i = 0; i < TETROMINO_TYPE_COUNT; i++) {
        put_enum(body, &off, (int)state->bag_state.bag1[i], TETROMINO_TYPE_COUNT);
    }
    for (int i = 0; i < TETROMINO_TYPE_COUNT; i++) {
        put_enum(body, &off, (int)state->bag_state.bag2[i], TETROMINO_TYPE_COUNT);
    }
    put_enum(body, &off, state->bag_state.bag1_offset, TETROMINO_TYPE_COUNT);

    put_i32(body, &off, state->garbage_balance);
    put_i32(body, &off, state->back_to_back_count);
    put_i32(body, &off, state->game_score);
    put_u8(body, &off, state->is_game_active ? 1u : 0u);

    assert(off == PROTO_STATE_REQUEST_BODY_LEN && "encoder disagrees with the layout table");
}

int proto_serialize_state_request(const ProtoStateRequest* state, unsigned char** body, size_t* body_len) {
    unsigned char* const buffer = malloc(PROTO_STATE_REQUEST_BODY_LEN);
    if (buffer == NULL) {
        return -1;
    }
    proto_encode_state_request(state, buffer);

    *body = buffer;
    *body_len = PROTO_STATE_REQUEST_BODY_LEN;
    return 0;
}

int proto_parse_move_request(const unsigned char* body, size_t body_len, ProtoMoveRequest* move) {
    const int idx = parse_token(body, body_len, MOVE_TOKEN, sizeof(MOVE_TOKEN) / sizeof(*MOVE_TOKEN));
    if (idx == -1) {
        return -1;
    }
    *move = (ProtoMoveRequest)idx;
    return 0;
}

const unsigned char* proto_serialize_move_request(ProtoMoveRequest move, size_t* body_len) {
    return serialize_token((size_t)move, MOVE_TOKEN, sizeof(MOVE_TOKEN) / sizeof(*MOVE_TOKEN), body_len);
}

int proto_parse_rotate_request(const unsigned char* body, size_t body_len, ProtoRotateRequest* rotate) {
    const int idx = parse_token(body, body_len, ROTATE_TOKEN, sizeof(ROTATE_TOKEN) / sizeof(*ROTATE_TOKEN));
    if (idx == -1) {
        return -1;
    }
    *rotate = (ProtoRotateRequest)idx;
    return 0;
}

const unsigned char* proto_serialize_rotate_request(ProtoRotateRequest rotate, size_t* body_len) {
    return serialize_token((size_t)rotate, ROTATE_TOKEN, sizeof(ROTATE_TOKEN) / sizeof(*ROTATE_TOKEN), body_len);
}

int proto_parse_drop_request(const unsigned char* body, size_t body_len, ProtoDropRequest* drop) {
    const int idx = parse_token(body, body_len, DROP_TOKEN, sizeof(DROP_TOKEN) / sizeof(*DROP_TOKEN));
    if (idx == -1) {
        return -1;
    }
    *drop = (ProtoDropRequest)idx;
    return 0;
}

const unsigned char* proto_serialize_drop_request(ProtoDropRequest drop, size_t* body_len) {
    return serialize_token((size_t)drop, DROP_TOKEN, sizeof(DROP_TOKEN) / sizeof(*DROP_TOKEN), body_len);
}
