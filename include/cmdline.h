#ifndef TETRISH_CMDLINE_H
#define TETRISH_CMDLINE_H

#include <stddef.h>

#define CMDLINE_MAX_ARGS 64

typedef enum {
    CMDLINE_OK,
    CMDLINE_ERR_UNTERMINATED_QUOTE,
    CMDLINE_ERR_TRAILING_BACKSLASH,
    CMDLINE_ERR_TOO_MANY_ARGS,
    CMDLINE_ERR_NO_MEMORY,
} CmdlineStatus;

/*!
    @brief split @p line into an argument vector, honouring `'...'` (literal,
           no escape processing) and `"..."` (escapes processed) quoting.

    Unquoted whitespace separates tokens. A backslash outside single quotes
    escapes the next character; inside double quotes `\\`, `\"`, `\n`, `\r`,
    `\t` map to the obvious characters and any other escaped character stands
    for itself. Quotes concatenate, so `a"b"'c'` is one token `abc`, and an
    empty quoted section still produces a token, so `''` is one empty argument.

    @pre  @p line is NUL-terminated and contains no embedded NUL to preserve
    @post on success @p out_argv holds @p out_argc heap strings that the caller
          releases with cmdline_free; on failure nothing is left allocated and
          @p out_argc is 0
    @return CMDLINE_OK, or the reason the line could not be split
*/
CmdlineStatus cmdline_split(const char* line, char* out_argv[CMDLINE_MAX_ARGS], size_t* out_argc);

/*!
    @brief free the @p argc strings cmdline_split wrote into @p argv

    @post every freed slot is set to NULL, so a double free is a no-op
*/
void cmdline_free(char* argv[CMDLINE_MAX_ARGS], size_t argc);

/*!
    @brief a human-readable phrase for @p status

    @return a string literal, never NULL
*/
const char* cmdline_status_string(CmdlineStatus status);

#endif
