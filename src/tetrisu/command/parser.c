#include "command/parser.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    QUOTE_NONE,
    QUOTE_SINGLE,
    QUOTE_DOUBLE,
} QuoteMode;

static void argv_cleanup(char** argv, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static int append_char(char** token, size_t* length, size_t* capacity, char ch) {
    if (*length + 1 >= *capacity) {
        const size_t next = *capacity == 0 ? 16 : *capacity * 2;
        char* resized = realloc(*token, next);
        if (resized == NULL) {
            return -1;
        }
        *token = resized;
        *capacity = next;
    }
    (*token)[(*length)++] = ch;
    return 0;
}

static CommandParseResult finish_token(
    char*** argv,
    size_t* argc,
    char** token,
    size_t* token_len,
    size_t* token_capacity
) {
    if (*argc == COMMAND_MAX_ARGS) {
        return COMMAND_PARSE_TOO_MANY_ARGS;
    }
    if (append_char(token, token_len, token_capacity, '\0') == -1) {
        return COMMAND_PARSE_NOMEM;
    }
    (*argv)[*argc] = *token;
    ++*argc;
    *token = NULL;
    *token_len = 0;
    *token_capacity = 0;
    return COMMAND_PARSE_OK;
}

CommandParseResult command_argv_parse(
    const char* line,
    size_t len,
    CommandArgv* out
) {
    out->argv = NULL;
    out->argc = 0;
    if (line == NULL && len != 0) {
        return COMMAND_PARSE_EMPTY;
    }
    if (len > COMMAND_MAX_BYTES) {
        return COMMAND_PARSE_TOO_LONG;
    }

    char** argv = calloc(COMMAND_MAX_ARGS, sizeof(*argv));
    if (argv == NULL) {
        return COMMAND_PARSE_NOMEM;
    }

    size_t argc = 0;
    char* token = NULL;
    size_t token_len = 0;
    size_t token_capacity = 0;
    bool token_started = false;
    QuoteMode quote = QUOTE_NONE;
    CommandParseResult result = COMMAND_PARSE_OK;

    for (size_t i = 0; i < len; ++i) {
        const char ch = line[i];
        if (quote == QUOTE_NONE && (ch == ' ' || ch == '\t')) {
            if (token_started) {
                result = finish_token(
                    &argv, &argc, &token, &token_len, &token_capacity
                );
                if (result != COMMAND_PARSE_OK) {
                    goto fail;
                }
                token_started = false;
            }
            continue;
        }

        if (quote == QUOTE_NONE && (ch == '\'' || ch == '"')) {
            quote = ch == '\'' ? QUOTE_SINGLE : QUOTE_DOUBLE;
            token_started = true;
            continue;
        }
        if ((quote == QUOTE_SINGLE && ch == '\'') ||
            (quote == QUOTE_DOUBLE && ch == '"')) {
            quote = QUOTE_NONE;
            continue;
        }

        char decoded = ch;
        if (quote == QUOTE_DOUBLE && ch == '\\') {
            if (++i == len) {
                result = COMMAND_PARSE_BAD_ESCAPE;
                goto fail;
            }
            switch (line[i]) {
            case '\\': decoded = '\\'; break;
            case '"': decoded = '"'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            default:
                result = COMMAND_PARSE_BAD_ESCAPE;
                goto fail;
            }
        }

        if (append_char(&token, &token_len, &token_capacity, decoded) == -1) {
            result = COMMAND_PARSE_NOMEM;
            goto fail;
        }
        token_started = true;
    }

    if (quote != QUOTE_NONE) {
        result = COMMAND_PARSE_UNTERMINATED_QUOTE;
        goto fail;
    }
    if (token_started) {
        result = finish_token(&argv, &argc, &token, &token_len, &token_capacity);
        if (result != COMMAND_PARSE_OK) {
            goto fail;
        }
    }
    if (argc == 0) {
        argv_cleanup(argv, argc);
        return COMMAND_PARSE_EMPTY;
    }

    out->argv = argv;
    out->argc = argc;
    return COMMAND_PARSE_OK;

fail:
    free(token);
    argv_cleanup(argv, argc);
    return result;
}

void command_argv_free(CommandArgv* argv) {
    argv_cleanup(argv->argv, argv->argc);
    argv->argv = NULL;
    argv->argc = 0;
}
