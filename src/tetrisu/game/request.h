#ifndef TETRISH_TETRISU_GAME_REQUEST_H
#define TETRISH_TETRISU_GAME_REQUEST_H

#include "game/intent.h"
#include "net/htttp_codec.h"

/*!
    @brief map one gameplay intent to tetrisd's HTTTP request contract

    @pre @p out is non-NULL
    @post on success, @p out borrows only string literals and shared protocol
          token storage; the caller owns nothing and may encode it immediately
    @post on failure, @p out is zeroed

    @return `0` for a named @c GameIntentType, `-1` otherwise
*/
int game_request_from_intent(GameIntentType intent, ClientRequest* out);

#endif
