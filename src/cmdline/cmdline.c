#include "cmdline.h"
#include "dtor.h"
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void free_strv(char*** argv_ptr) {
    if (*argv_ptr == NULL) return;
    for (char** arg = *argv_ptr; *arg != NULL; arg++) free(*arg);
    free(*argv_ptr);
}
static DTOR_WRAPPER_DEFINE(free_strv)
static DTOR_WRAPPER_DEFINE(free)

static bool decode_dollar_escape(char c, char* out) {
    switch (c) {
    case 'n': *out = '\n'; return true;
    case 'r': *out = '\r'; return true;
    case 't': *out = '\t'; return true;
    case 'a': *out = '\a'; return true;
    case 'b': *out = '\b'; return true;
    case 'f': *out = '\f'; return true;
    case 'v': *out = '\v'; return true;
    case '\\':
    case '\'':
    case '"': *out = c; return true;
    default: return false;
    }
}

CmdlineResult cmdline_parse(const char* content, char*** out_args, size_t* out_len) {
    DTOR_DEFINE(errdtor, 1);
    DTOR_DEFINE(dtor, 1);

    *out_args = NULL;
    *out_len = 0;

    // an arg can only shrink relative to its source text, so one buffer of
    // the whole line's length fits any single arg.
    char* const buf = malloc(strlen(content) + 1);
    if (buf == NULL) return CMDLINE_ERR_NOMEM;
    DTOR_INSERT(dtor, free, buf);

    size_t argc = 0;
    size_t cap = 8;
    char** argv = malloc(cap * sizeof *argv);
    if (argv == NULL) DTOR_RETURN(dtor, CMDLINE_ERR_NOMEM);
    argv[0] = NULL;
    DTOR_INSERT(errdtor, free_strv, &argv);

    const char* p = content;
    while (*p != '\0') {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        // line continuation between tokens must not start an (empty) token
        if (p[0] == '\\' && p[1] == '\n') {
            p += 2;
            continue;
        }

        size_t len = 0;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            if (*p == '\'' || *p == '"') {
                const char quote = *p++;
                while (*p != '\0' && *p != quote) buf[len++] = *p++;
                if (*p == '\0') DTOR_ERR_RETURN(errdtor, dtor, CMDLINE_ERR_UNTERMINATED);
                p++;
            } else if (p[0] == '$' && p[1] == '\'') {
                p += 2;
                while (*p != '\0' && *p != '\'') {
                    if (*p == '\\' && p[1] != '\0') {
                        char decoded;
                        if (decode_dollar_escape(p[1], &decoded)) {
                            buf[len++] = decoded;
                        } else {
                            buf[len++] = p[0];
                            buf[len++] = p[1];
                        }
                        p += 2;
                    } else {
                        buf[len++] = *p++;
                    }
                }
                if (*p == '\0') DTOR_ERR_RETURN(errdtor, dtor, CMDLINE_ERR_UNTERMINATED);
                p++;
            } else if (*p == '\\') {
                if (p[1] == '\0') {
                    buf[len++] = *p++;
                } else if (p[1] == '\n') {
                    p += 2;
                } else {
                    buf[len++] = p[1];
                    p += 2;
                }
            } else {
                buf[len++] = *p++;
            }
        }

        if (argc + 2 > cap) {
            cap *= 2;
            char** const new_argv = realloc(argv, cap * sizeof *argv);
            if (new_argv == NULL) DTOR_ERR_RETURN(errdtor, dtor, CMDLINE_ERR_NOMEM);
            argv = new_argv;
        }
        char* const arg = malloc(len + 1);
        if (arg == NULL) DTOR_ERR_RETURN(errdtor, dtor, CMDLINE_ERR_NOMEM);
        memcpy(arg, buf, len);
        arg[len] = '\0';
        argv[argc++] = arg;
        argv[argc] = NULL;
    }

    *out_args = argv;
    *out_len = argc;
    DTOR_RETURN(dtor, CMDLINE_OK);
}
