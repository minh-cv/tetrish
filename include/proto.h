/*!
    @file proto.h
    @brief Conversion between HTTTP message bodies and the concrete domain
    types they carry.

    Each function takes exactly the part of the message its own encoding
    needs, decided per function rather than by a shape imposed on all of them.
    Today that is the body alone for all four — a `const unsigned char*` and
    its length — so nothing here currently reads or writes a method, path,
    status or header, and routing belongs to the caller. A type whose encoding
    later needs more of the message takes more of it.

    All four types travel in HTTTP *requests*, which is what the `Request`
    suffix records: `MOVE`, `ROTATE` and `DROP` go from player to server, and
    `STATE` is pushed the other way as a server-originated request (see
    `docs/tetrisu/repl.md`).

    ## Parsing

    A parse function assumes the caller has already established that the
    message really carries this type, by whatever routing check the caller
    uses. A body does not identify its own type and these functions do not
    verify it: `proto_parse_rotate_request` accepts the body of a move request
    spelled `"CW"` as readily as a rotate one. Supplying the wrong body is a
    caller bug rather than a malformed message, and the -1 return does not
    tell the two apart.

    A parse function returns -1 on a body that does not encode its type and
    leaves its output parameter unmodified.

    A parse result is a value copy. It never points into @p body, so it stays
    valid once the frame buffer the body came from is reclaimed. This is
    restated per function; it is a property of these four types, not a promise
    covering everything added later.

    ## Serializing

    Serialization has no precondition beyond the argument being a named
    enumerator or an initialized struct.

    Ownership of a produced body is fixed per function and stated there, so
    none of them reports it back at run time. A body is heap allocated unless
    there is static storage to point at: `proto_serialize_state_request` builds
    a buffer and hands it over, while the three token bodies are string
    literals.

    A heap body is yours — writable, and freed either by you or by
    `htttp_message_free` if you set `HtttpMessageOwnership.is_body_owned` when
    attaching it. A static body comes back `const` because writing to it would
    corrupt every later call that returns the same storage; never free it and
    never set `is_body_owned` on it.

    Bodies are `unsigned char`, while `htttp_make_default_response` takes
    `const char*`; that one cast is the caller's.
*/

#ifndef TETRISH_PROTO_H
#define TETRISH_PROTO_H

#include "tetrisbrain/state.h"
#include "tetrisbrain/static.h"

#include <stdbool.h>
#include <stddef.h>

/*!
    @brief A snapshot of one player's game, as pushed in a `STATE` request.

    Deliberately a subset of @c State rather than an alias for it: only what a
    client needs to render and predict crosses the wire, and the two are free
    to drift. Notably absent is @c State.spawn_type , which is redundant —
    the piece in play is `bag_state.bag1[bag_state.bag1_offset]` and the queue
    after it continues through @c bag1 then @c bag2 .

    @note @c game_score and @c is_game_active have no counterpart in @c State ;
          the server fills them from the room's scoring and status.
*/
typedef struct {
    BoardState board_state;
    int combo_counter;
    HoldState hold_state;
    BagState bag_state;
    int garbage_balance;
    int back_to_back_count;
    int game_score;
    bool is_game_active;
} ProtoStateRequest;

//! @brief Body `"LEFT"` or `"RIGHT"`.
typedef enum {
    PROTO_MOVE_LEFT,
    PROTO_MOVE_RIGHT,
} ProtoMoveRequest;

//! @brief Body `"CW"` or `"CCW"`.
typedef enum {
    PROTO_ROTATE_CW,
    PROTO_ROTATE_CCW,
} ProtoRotateRequest;

//! @brief Body `"SOFT"` or `"HARD"`.
typedef enum {
    PROTO_DROP_SOFT,
    PROTO_DROP_HARD,
} ProtoDropRequest;

/*!
    @brief Encoded size of an @c ProtoStateRequest body, in bytes.

    The encoding is fixed-width, so this is both the exact length
    `proto_serialize_state_request` produces and the only length
    `proto_parse_state_request` accepts.

    | offset | size | field |
    |---|---|---|
    | 0   | 400 | `board_state.board.cells`, row-major, one byte per cell |
    | 400 | 1   | `board_state.tetromino.type` |
    | 401 | 1   | `board_state.tetromino.direction` |
    | 402 | 4   | `board_state.tetromino.col_offset` |
    | 406 | 4   | `board_state.tetromino.row_offset` |
    | 410 | 4   | `board_state.ghost.ghost_col_offset` |
    | 414 | 4   | `board_state.ghost.ghost_row_offset` |
    | 418 | 4   | `combo_counter` |
    | 422 | 1   | `hold_state.hold_type` |
    | 423 | 1   | `hold_state.hold_status` |
    | 424 | 7   | `bag_state.bag1` |
    | 431 | 7   | `bag_state.bag2` |
    | 438 | 1   | `bag_state.bag1_offset` |
    | 439 | 4   | `garbage_balance` |
    | 443 | 4   | `back_to_back_count` |
    | 447 | 4   | `game_score` |
    | 451 | 1   | `is_game_active`, 0 or 1 |

    Every 4-byte field is a two's-complement big-endian `int32_t`; negative
    values are expected, since @c combo_counter and @c back_to_back_count
    both start at -1. Every 1-byte field is an enumerator's ordinal, except
    @c bag1_offset (an index) and @c is_game_active (a boolean). Nothing is
    written as a host struct, so padding, enum width and byte order stay off
    the wire.
*/
#define PROTO_STATE_REQUEST_BODY_LEN                        \
    ((size_t)(BOARD_HEIGHT) * (size_t)(BOARD_WIDTH) + 2u + \
     4u * 4u + 4u + 2u + 2u * (size_t)(TETROMINO_TYPE_COUNT) + 1u + 4u + 4u + 4u + 1u)

/*!
    @brief Decode the body of a `STATE` request.

    @pre @p body holds @p body_len readable bytes, or is NULL with
         @p body_len `0`.

    @post On success @p state holds the decoded snapshot, sharing no storage
          with @p body .
    @post On failure @p state is unmodified.

    @return -1 if @p body_len is not @c PROTO_STATE_REQUEST_BODY_LEN or any
            enumerator, bag index or boolean byte is out of range, 0
            otherwise.

    @note Geometry is not validated: a tetromino or ghost offset placing the
          piece off the board decodes fine, since @c is_outside_playfield_area
          makes that a legitimate transient state rather than a wire error.
*/
int proto_parse_state_request(const unsigned char* body, size_t body_len, ProtoStateRequest* state);

/*!
    @brief Encode a `STATE` request body into caller-supplied storage.

    The allocation-free half of `proto_serialize_state_request`, for a server
    pushing state to every player each tick.

    @pre @p state is initialized and every enumerator, bag index and offset in
         it is in range; violations are asserted in debug builds and encode to
         truncated bytes in release ones.
    @pre @p body is writable for @c PROTO_STATE_REQUEST_BODY_LEN bytes.

    @post @p body holds the whole encoding; every byte is written.
*/
void proto_encode_state_request(const ProtoStateRequest* state, unsigned char body[PROTO_STATE_REQUEST_BODY_LEN]);

/*!
    @brief Encode a `STATE` request body into a fresh heap buffer.

    @pre as @c proto_encode_state_request .

    @post On success @p body holds a heap buffer of @p body_len
          (@c PROTO_STATE_REQUEST_BODY_LEN) bytes, owned by the caller and
          writable.
    @post On failure both output parameters are unmodified and nothing was
          allocated, so the caller has nothing to release.

    @return -1 if the allocation failed, 0 otherwise.
*/
int proto_serialize_state_request(const ProtoStateRequest* state, unsigned char** body, size_t* body_len);

/*!
    @brief Decode the body of a `MOVE` request.

    Matching is exact and case-sensitive: `"LEFT"` and `"RIGHT"` alone, with
    no surrounding whitespace and no trailing newline.

    @pre @p body holds @p body_len readable bytes, or is NULL with
         @p body_len `0`.

    @post On success @p move holds the decoded direction, sharing no storage
          with @p body .
    @post On failure @p move is unmodified.

    @return -1 if the body is not one of the two spellings, 0 otherwise.
*/
int proto_parse_move_request(const unsigned char* body, size_t body_len, ProtoMoveRequest* move);

/*!
    @brief Produce the body of a `MOVE` request.

    @pre @p move is a named enumerator; anything else is asserted in debug
         builds and encodes as @c PROTO_MOVE_LEFT in release ones.

    @post @p body_len holds the body's length.

    @return a string literal of @p body_len bytes, never NULL. It is static
            storage shared with every other call: do not write to it and do
            not free it. Treat it as unterminated, though it does happen to
            carry a NUL.
*/
const unsigned char* proto_serialize_move_request(ProtoMoveRequest move, size_t* body_len);

/*!
    @brief Decode the body of a `ROTATE` request.

    @see proto_parse_move_request for match rules; the spellings are `"CW"` and
    `"CCW"`.
*/
int proto_parse_rotate_request(const unsigned char* body, size_t body_len, ProtoRotateRequest* rotate);

/*!
    @brief Produce the body of a `ROTATE` request.

    @see proto_serialize_move_request ; an out-of-range @p rotate encodes as
    @c PROTO_ROTATE_CW in release builds.
*/
const unsigned char* proto_serialize_rotate_request(ProtoRotateRequest rotate, size_t* body_len);

/*!
    @brief Decode the body of a `DROP` request.

    @see proto_parse_move_request for match rules; the spellings are `"SOFT"`
    and `"HARD"`.
*/
int proto_parse_drop_request(const unsigned char* body, size_t body_len, ProtoDropRequest* drop);

/*!
    @brief Produce the body of a `DROP` request.

    @see proto_serialize_move_request ; an out-of-range @p drop encodes as
    @c PROTO_DROP_SOFT in release builds.
*/
const unsigned char* proto_serialize_drop_request(ProtoDropRequest drop, size_t* body_len);

#endif
