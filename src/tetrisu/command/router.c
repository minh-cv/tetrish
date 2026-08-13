#include "command/router.h"

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
    } else if (strcmp(name, "set-name") == 0) {
        if (argv->argc < 2) {
            return COMMAND_ROUTE_MISSING_ARGUMENT;
        }
        out->type = COMMAND_SET_NAME;
        if (join_arguments(argv, 1, out) == -1) {
            return COMMAND_ROUTE_NOMEM;
        }
    } else if (strcmp(name, "whoami") == 0) {
        out->type = COMMAND_WHOAMI;
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
