#include "command/router.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int join_arguments(const CommandArgv* argv, size_t start, ParsedCommand* out) {
    size_t total = 0;
    for (size_t i = start; i < argv->argc; ++i) {
        const size_t length = strlen(argv->argv[i]);
        if (length > SIZE_MAX - total - (i != start ? 1u : 0u)) {
            return -1;
        }
        total += length + (i != start ? 1u : 0u);
    }

    char* argument = malloc(total + 1);
    if (argument == NULL) {
        return -1;
    }
    size_t used = 0;
    for (size_t i = start; i < argv->argc; ++i) {
        if (i != start) {
            argument[used++] = ' ';
        }
        const size_t length = strlen(argv->argv[i]);
        memcpy(argument + used, argv->argv[i], length);
        used += length;
    }
    argument[used] = '\0';
    out->argument = argument;
    out->argument_len = used;
    return 0;
}

static CommandRouteResult route_no_argument(
    const CommandArgv* argv,
    ParsedCommand* out,
    GameIntentType intent
) {
    if (argv->argc != 1) {
        return COMMAND_ROUTE_TOO_MANY_ARGUMENTS;
    }
    out->type = COMMAND_GAME;
    out->game_intent = intent;
    return COMMAND_ROUTE_OK;
}

static CommandRouteResult route_choice(
    const CommandArgv* argv,
    ParsedCommand* out,
    const char* first,
    GameIntentType first_intent,
    const char* second,
    GameIntentType second_intent
) {
    if (argv->argc < 2) {
        return COMMAND_ROUTE_MISSING_ARGUMENT;
    }
    if (argv->argc > 2) {
        return COMMAND_ROUTE_TOO_MANY_ARGUMENTS;
    }
    if (strcmp(argv->argv[1], first) == 0) {
        out->type = COMMAND_GAME;
        out->game_intent = first_intent;
        return COMMAND_ROUTE_OK;
    }
    if (strcmp(argv->argv[1], second) == 0) {
        out->type = COMMAND_GAME;
        out->game_intent = second_intent;
        return COMMAND_ROUTE_OK;
    }
    return COMMAND_ROUTE_INVALID_ARGUMENT;
}

static bool is_decimal(const char* text) {
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9') {
            return false;
        }
    }
    return true;
}

/*!
    @brief route a command taking exactly one argument, kept verbatim

    The argument reaches the request builder as text; only its shape is
    checked here, so a malformed room id is refused without spending a
    round trip on it.
*/
static CommandRouteResult route_one_argument(
    const CommandArgv* argv,
    ParsedCommand* out,
    GameIntentType intent,
    bool (*is_valid)(const char*)
) {
    if (argv->argc < 2) {
        return COMMAND_ROUTE_MISSING_ARGUMENT;
    }
    if (argv->argc > 2) {
        return COMMAND_ROUTE_TOO_MANY_ARGUMENTS;
    }
    if (!is_valid(argv->argv[1])) {
        return COMMAND_ROUTE_INVALID_ARGUMENT;
    }
    out->type = COMMAND_GAME;
    out->game_intent = intent;
    if (join_arguments(argv, 1, out) == -1) {
        return COMMAND_ROUTE_NOMEM;
    }
    return COMMAND_ROUTE_OK;
}

/*!
    @brief route `create` and its room options

    Options are matched by shape rather than by a flag grammar, which suits
    a command line typed during play: a number is the seat count, and each
    remaining word names a flag. A bare @c create keeps its argument NULL so
    the request carries no body and the server's own defaults apply.
*/
static CommandRouteResult route_create(const CommandArgv* argv, ParsedCommand* out) {
    for (size_t i = 1; i < argv->argc; ++i) {
        const char* option = argv->argv[i];
        if (!is_decimal(option) &&
            strcmp(option, "public") != 0 && strcmp(option, "cross") != 0) {
            return COMMAND_ROUTE_INVALID_ARGUMENT;
        }
    }
    out->type = COMMAND_GAME;
    out->game_intent = GAME_INTENT_CREATE;
    if (argv->argc > 1 && join_arguments(argv, 1, out) == -1) {
        return COMMAND_ROUTE_NOMEM;
    }
    return COMMAND_ROUTE_OK;
}

CommandRouteResult command_route(const CommandArgv* argv, ParsedCommand* out) {
    memset(out, 0, sizeof(*out));
    if (argv == NULL || argv->argc == 0) {
        return COMMAND_ROUTE_UNKNOWN;
    }

    const char* name = argv->argv[0];
    if (strcmp(name, "help") == 0) {
        out->type = COMMAND_HELP;
    } else if (strcmp(name, "quit") == 0 || strcmp(name, "exit") == 0) {
        out->type = COMMAND_QUIT;
    } else if (strcmp(name, "reconnect") == 0) {
        out->type = COMMAND_RECONNECT;
    } else if (strcmp(name, "disconnect") == 0) {
        out->type = COMMAND_DISCONNECT;
    } else if (strcmp(name, "create") == 0) {
        return route_create(argv, out);
    } else if (strcmp(name, "join") == 0) {
        return route_one_argument(argv, out, GAME_INTENT_JOIN, is_decimal);
    } else if (strcmp(name, "start") == 0) {
        return route_no_argument(argv, out, GAME_INTENT_START);
    } else if (strcmp(name, "move") == 0) {
        return route_choice(
            argv, out, "left", GAME_INTENT_MOVE_LEFT,
            "right", GAME_INTENT_MOVE_RIGHT
        );
    } else if (strcmp(name, "rotate") == 0) {
        return route_choice(
            argv, out, "cw", GAME_INTENT_ROTATE_CW,
            "ccw", GAME_INTENT_ROTATE_CCW
        );
    } else if (strcmp(name, "drop") == 0) {
        return route_choice(
            argv, out, "soft", GAME_INTENT_DROP_SOFT,
            "hard", GAME_INTENT_DROP_HARD
        );
    } else if (strcmp(name, "hold") == 0) {
        return route_no_argument(argv, out, GAME_INTENT_HOLD);
    } else if (strcmp(name, "leave") == 0) {
        return route_no_argument(argv, out, GAME_INTENT_LEAVE);
    } else if (strcmp(name, "set-name") == 0 || strcmp(name, "whoami") == 0) {
        out->type = COMMAND_UNSUPPORTED;
    } else if (strcmp(name, "htttp") == 0) {
        if (argv->argc < 2) {
            return COMMAND_ROUTE_MISSING_ARGUMENT;
        }
        out->type = COMMAND_SEND_RAW;
        if (join_arguments(argv, 1, out) == -1) {
            return COMMAND_ROUTE_NOMEM;
        }
    } else {
        return COMMAND_ROUTE_UNKNOWN;
    }
    return COMMAND_ROUTE_OK;
}

void parsed_command_free(ParsedCommand* command) {
    free(command->argument);
    memset(command, 0, sizeof(*command));
}
