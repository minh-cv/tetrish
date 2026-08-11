#include "cmdline.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool is_separator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/*
    Everything unlisted stands for itself, so `\ ` is a literal space and `\\`
    a literal backslash without either needing a case here. `\0` is deliberately
    absent: a NUL would truncate the token it appears in.
*/
static char unescape(char c) {
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'e': return '\033';
    default: return c;
    }
}

CmdlineStatus cmdline_split(const char* line, char* out_argv[CMDLINE_MAX_ARGS], size_t* out_argc) {
    *out_argc = 0;

    // no escape and no quote lengthens a token, so one buffer this size serves
    // every token of the line
    char* const scratch = malloc(strlen(line) + 1);
    if (scratch == NULL) {
        return CMDLINE_ERR_NO_MEMORY;
    }

    CmdlineStatus status = CMDLINE_OK;
    const char* p = line;
    size_t argc = 0;

    while (status == CMDLINE_OK) {
        while (is_separator(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        size_t used = 0;
        bool token_done = false;
        while (!token_done && status == CMDLINE_OK) {
            if (*p == '\0' || is_separator(*p)) {
                token_done = true;
                continue;
            }

            switch (*p) {
            case '\'':
                p++;
                while (*p != '\'' && *p != '\0') {
                    scratch[used++] = *p++;
                }
                if (*p == '\0') {
                    status = CMDLINE_ERR_UNTERMINATED_QUOTE;
                }
                else {
                    p++;
                }
                break;

            case '"':
                p++;
                while (*p != '"' && *p != '\0') {
                    if (*p != '\\') {
                        scratch[used++] = *p++;
                        continue;
                    }
                    p++;
                    if (*p == '\0') {
                        status = CMDLINE_ERR_TRAILING_BACKSLASH;
                        break;
                    }
                    scratch[used++] = unescape(*p++);
                }
                if (status != CMDLINE_OK) {
                    break;
                }
                if (*p == '\0') {
                    status = CMDLINE_ERR_UNTERMINATED_QUOTE;
                }
                else {
                    p++;
                }
                break;

            case '\\':
                p++;
                if (*p == '\0') {
                    status = CMDLINE_ERR_TRAILING_BACKSLASH;
                    break;
                }
                scratch[used++] = unescape(*p++);
                break;

            default:
                scratch[used++] = *p++;
                break;
            }
        }
        if (status != CMDLINE_OK) {
            break;
        }

        if (argc == CMDLINE_MAX_ARGS) {
            status = CMDLINE_ERR_TOO_MANY_ARGS;
            break;
        }

        char* const token = malloc(used + 1);
        if (token == NULL) {
            status = CMDLINE_ERR_NO_MEMORY;
            break;
        }
        memcpy(token, scratch, used);
        token[used] = '\0';
        out_argv[argc++] = token;
    }

    free(scratch);

    if (status != CMDLINE_OK) {
        cmdline_free(out_argv, argc);
        return status;
    }

    *out_argc = argc;
    return CMDLINE_OK;
}

void cmdline_free(char* argv[CMDLINE_MAX_ARGS], size_t argc) {
    for (size_t i = 0; i < argc; i++) {
        free(argv[i]);
        argv[i] = NULL;
    }
}

const char* cmdline_status_string(CmdlineStatus status) {
    switch (status) {
    case CMDLINE_OK: return "ok";
    case CMDLINE_ERR_UNTERMINATED_QUOTE: return "unterminated quote";
    case CMDLINE_ERR_TRAILING_BACKSLASH: return "trailing backslash";
    case CMDLINE_ERR_TOO_MANY_ARGS: return "too many arguments";
    case CMDLINE_ERR_NO_MEMORY: return "out of memory";
    }
    return "unknown error";
}
