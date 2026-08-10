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

static char decode_escape(char c) {
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    default: return c;
    }
}

char** cmdline_parse(const char* content, size_t* out_len) {
    DTOR_DEFINE(errdtor, 1);
    DTOR_DEFINE(dtor, 1);

    *out_len = 0;

    // an arg can only shrink relative to its source text, so one buffer of
    // the whole line's length fits any single arg.
    char* const buf = malloc(strlen(content) + 1);
    if (buf == NULL) return NULL;
    DTOR_INSERT(dtor, free, buf);

    size_t argc = 0;
    size_t cap = 8;
    char** argv = malloc(cap * sizeof *argv);
    if (argv == NULL) DTOR_RETURN(dtor, NULL);
    argv[0] = NULL;
    DTOR_INSERT(errdtor, free_strv, &argv);

    const char* p = content;
    while (*p != '\0') {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }

        size_t len = 0;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            if (*p == '\'') {
                p++;
                while (*p != '\0' && *p != '\'') buf[len++] = *p++;
                if (*p == '\0') DTOR_ERR_RETURN(errdtor, dtor, NULL);
                p++;
            } else if (*p == '"') {
                p++;
                while (*p != '\0' && *p != '"') {
                    if (*p == '\\' && p[1] != '\0') {
                        buf[len++] = decode_escape(p[1]);
                        p += 2;
                    } else {
                        buf[len++] = *p++;
                    }
                }
                if (*p == '\0') DTOR_ERR_RETURN(errdtor, dtor, NULL);
                p++;
            } else if (*p == '\\' && p[1] != '\0') {
                buf[len++] = decode_escape(p[1]);
                p += 2;
            } else {
                buf[len++] = *p++;
            }
        }

        if (argc + 2 > cap) {
            cap *= 2;
            char** const new_argv = realloc(argv, cap * sizeof *argv);
            if (new_argv == NULL) DTOR_ERR_RETURN(errdtor, dtor, NULL);
            argv = new_argv;
        }
        char* const arg = malloc(len + 1);
        if (arg == NULL) DTOR_ERR_RETURN(errdtor, dtor, NULL);
        memcpy(arg, buf, len);
        arg[len] = '\0';
        argv[argc++] = arg;
        argv[argc] = NULL;
    }

    *out_len = argc;
    DTOR_RETURN(dtor, argv);
}
