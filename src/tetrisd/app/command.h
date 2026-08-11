#ifndef TETRISH_TETRISD_APP_COMMAND_H
#define TETRISH_TETRISD_APP_COMMAND_H

#include "htttp.h"
#include <stddef.h>

/*!
    @brief The methods the daemon routes.

    JOIN, LEAVE, START, MOVE, ROTATE and DROP are the methods the spec fixes.
    SET_PLAYER_NAME, WHOAMI and HOLD are additions of this implementation:
    the spec gives no way to carry a display name, and libtetrisbrain
    implements a hold piece that no fixed method reaches. They are documented
    alongside the rest in the README.
*/
typedef enum {
    APP_COMMAND_SET_NAME,   // SET_PLAYER_NAME /player, body is the name
    APP_COMMAND_WHOAMI,     // WHOAMI /player
    APP_COMMAND_JOIN,       // JOIN /room/<id>
    APP_COMMAND_LEAVE,      // LEAVE /room/<id>
    APP_COMMAND_START,      // START /room/<id>
    APP_COMMAND_MOVE,       // MOVE /room/<id>/player/<pid>, body LEFT|RIGHT
    APP_COMMAND_ROTATE,     // ROTATE /room/<id>/player/<pid>, body CW|CCW
    APP_COMMAND_DROP,       // DROP /room/<id>/player/<pid>, body SOFT|HARD
    APP_COMMAND_HOLD,       // HOLD /room/<id>/player/<pid>
} AppCommandKind;

/*!
    @brief Why a message could not be turned into a command. Each maps to one
    status code, which is the only reason the distinctions exist.
*/
typedef enum {
    APP_COMMAND_OK,
    APP_COMMAND_ERR_NOT_A_REQUEST,   // 400: the peer sent a response
    APP_COMMAND_ERR_UNKNOWN_METHOD,  // 404: no such method
    APP_COMMAND_ERR_BAD_PATH,        // 404: the path does not fit the method
    APP_COMMAND_ERR_WRONG_PLAYER,    // 403: Player-Id names someone else
} AppCommandStatus;

/*!
    @brief A parsed request. Every string member is a non-owning view into the
    message, which itself views the decrypted frame, so a command must not
    outlive the tick that parsed it.
*/
typedef struct {
    AppCommandKind kind;
    const char* room_id;    // NULL when the method takes no room
    size_t room_id_len;
    long player_id;         // -1 when the path carries none
    const char* body;       // never NULL; empty bodies give an empty range
    size_t body_len;
} AppCommand;

/*!
    @brief route @p message into @p out

    The `Player-Id` header the spec requires on authenticated requests is
    checked here: a request that names an id other than @p self_id is
    rejected rather than applied to the connection that actually sent it.
    The sentinel `-1` is accepted, which is what a client sends before it has
    learned its id from a first response.

    @post on APP_COMMAND_OK, @p out is filled; otherwise it is untouched
*/
AppCommandStatus app_command_parse(const HtttpMessage* message, long self_id, AppCommand* out);

/*!
    @brief the status code a parse failure answers with
*/
HtttpStatus app_command_status_code(AppCommandStatus status);

/*!
    @brief a human-readable phrase for @p status, suitable as a response body

    @return a string literal, never NULL
*/
const char* app_command_status_reason(AppCommandStatus status);

#endif
