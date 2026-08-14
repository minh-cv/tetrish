#ifndef TETRISH_TETRISU_GAME_REQUEST_H
#define TETRISH_TETRISU_GAME_REQUEST_H

#include "game/intent.h"
#include "net/htttp_codec.h"

/*!
    @brief Caller-owned room for the fields an intent cannot borrow.

    A room id makes the path per-request and the room options make the body
    per-request, so neither can be a literal. The caller supplies the storage
    rather than the builder allocating, because the request is serialized
    immediately at submit and nothing retains these pointers afterwards.
*/
typedef struct {
    char path[32];
    char body[96];
} GameRequestScratch;

/*!
    @brief map one gameplay intent to tetrisd's HTTTP request contract

    @p argument is the command's argument text, or NULL when it had none:
    the room id for @c GAME_INTENT_JOIN , and the room options for
    @c GAME_INTENT_CREATE (a decimal seat count, and the words `public` and
    `cross`, in any order). A @c CREATE with no argument sends no body, so
    the server's own room defaults apply.

    @pre @p scratch and @p out are non-NULL
    @pre @p argument was shape-checked by the router

    @post on success, @p out borrows string literals, shared protocol token
          storage, and @p scratch ; the caller owns nothing and may encode it
          immediately
    @post on failure, @p out is zeroed

    @return `0` for a named @c GameIntentType, `-1` otherwise
*/
int game_request_from_intent(
    GameIntentType intent,
    const char* argument,
    GameRequestScratch* scratch,
    ClientRequest* out
);

#endif
