#ifndef TETRISH_TETRISD_APP_DISPATCH_H
#define TETRISH_TETRISD_APP_DISPATCH_H

#include "app/app_layer.h"
#include "htttp.h"
#include "type.h"

/*!
    @brief outcome of handling one parsed message
*/
typedef enum {
    //! @brief no response was built and @p outbound is untouched
    DISPATCH_ERR,
    //! @brief @p outbound holds a response owned by the caller
    DISPATCH_RESPOND,
    //! @brief the message needs no response; @p outbound is untouched
    DISPATCH_NO_RESPONSE,
} DispatchResult;

/*!
    @brief act on one request of @p fd , building its response into
           @p outbound

    Routing is on the method alone; see @c docs/tetrisd/api.md for the
    per-method status codes. An unknown method is a 405. @c MOVE ,
    @c ROTATE , @c DROP and @c HOLD carry no response and instead record a
    key in the pending inputs of @p fd 's room, for the next tick to apply.

    @pre @p fd is a key in @c players

    @post on @c DISPATCH_RESPOND , @p outbound holds a response whose
          memory is owned by the caller
    @post on @c DISPATCH_NO_RESPONSE , the room state of @p fd may have
          changed
*/
DispatchResult respond_one_request(
    AppData* data,
    Fd fd,
    const HtttpRequest* parsed,
    HtttpOutboundMessage* outbound
);

#endif
