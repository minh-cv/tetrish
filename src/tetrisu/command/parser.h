#ifndef TETRISH_TETRISU_COMMAND_PARSER_H
#define TETRISH_TETRISU_COMMAND_PARSER_H

#include <stddef.h>

#define COMMAND_MAX_BYTES 4096u
#define COMMAND_MAX_ARGS 64u

typedef struct {
    char** argv;
    size_t argc;
} CommandArgv;

typedef enum {
    COMMAND_PARSE_OK,
    COMMAND_PARSE_EMPTY,
    COMMAND_PARSE_TOO_LONG,
    COMMAND_PARSE_TOO_MANY_ARGS,
    COMMAND_PARSE_UNTERMINATED_QUOTE,
    COMMAND_PARSE_BAD_ESCAPE,
    COMMAND_PARSE_NOMEM,
} CommandParseResult;

/*!
    @brief split one command line into an owned argv using shell-like quotes

    Single quotes preserve their contents literally. Double quotes recognize
    `\\`, `\"`, `\n`, `\r` and `\t`. Quote delimiters are not copied.

    @pre @p line contains exactly @p len accessible bytes
    @pre @p out owns no argv allocation

    @post on success, @p out owns @c argc NUL-terminated strings
    @post on failure, @p out is empty

    @return a stable parse result; `COMMAND_PARSE_OK` is the only success value
*/
CommandParseResult command_argv_parse(
    const char* line,
    size_t len,
    CommandArgv* out
);

/*!
    @brief release every string and the argv allocation owned by @p argv

    @post @p argv is empty and may be freed again
*/
void command_argv_free(CommandArgv* argv);

#endif
