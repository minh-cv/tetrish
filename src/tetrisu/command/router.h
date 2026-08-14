#ifndef TETRISH_TETRISU_COMMAND_ROUTER_H
#define TETRISH_TETRISU_COMMAND_ROUTER_H

#include "command/parser.h"
#include "game/intent.h"

#include <stddef.h>

typedef enum {
    COMMAND_HELP,
    COMMAND_QUIT,
    COMMAND_RECONNECT,
    COMMAND_DISCONNECT,
    COMMAND_GAME,
    COMMAND_SEND_RAW,
    COMMAND_UNSUPPORTED,
    COMMAND_PAUSE,
} CommandType;

typedef struct {
    CommandType type;
    char* argument;
    size_t argument_len;
    GameIntentType game_intent;
} ParsedCommand;

typedef enum {
    COMMAND_ROUTE_OK,
    COMMAND_ROUTE_UNKNOWN,
    COMMAND_ROUTE_MISSING_ARGUMENT,
    COMMAND_ROUTE_INVALID_ARGUMENT,
    COMMAND_ROUTE_TOO_MANY_ARGUMENTS,
    COMMAND_ROUTE_NOMEM,
} CommandRouteResult;

/*!
    @brief convert parsed argv into one owned domain command

    @pre @p argv contains at least one argument
    @pre @p out owns no argument allocation

    @post on success, @p out owns any command argument
    @post on failure, @p out owns no allocation
    @post @p argv is unchanged
*/
CommandRouteResult command_route(const CommandArgv* argv, ParsedCommand* out);

/*!
    @brief free the argument owned by @p command

    @post @p command owns no allocation and may be freed again
*/
void parsed_command_free(ParsedCommand* command);

#endif
