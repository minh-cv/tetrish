#ifndef TETRISH_TETRISD_APP_EVENT_H
#define TETRISH_TETRISD_APP_EVENT_H

#include "app/world.h"
#include <stddef.h>

/*
    A STATE body is line-oriented `key=value` text plus one block of board
    rows. Text rather than JSON because the client parses one of these per
    broadcast per seat and a fixed grammar costs less than a document model on
    both sides; and because a body a human can read off a socket dump is worth
    a great deal when the protocol is the thing being graded.
*/
#define APP_STATE_BODY_MAX 4096

/*!
    @brief render @p room from @p seat 's point of view into @p out

    The board block covers the visible rows only, one character per cell:
    `I J L O S T Z` for a locked piece of that type, `#` for garbage, `.` for
    empty. The falling piece and its ghost are sent separately rather than
    stamped into the board, so the client can draw the ghost differently
    without having to guess which cells are which.

    @pre @p out has room for APP_STATE_BODY_MAX bytes
    @post @p out_len holds the length written
    @return -1 if the snapshot did not fit, in which case @p out is unusable
*/
int app_event_encode_state(const World* world, const Room* room, uint8_t seat,
                           char* out, size_t out_size, size_t* out_len);

/*!
    @brief render the membership of @p room into @p out

    Sent as the reply to JOIN, LEAVE and START, so a client learns the roster
    without a separate query.
*/
int app_event_encode_room(const World* world, const Room* room,
                          char* out, size_t out_size, size_t* out_len);

#endif
