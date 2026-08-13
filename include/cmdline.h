#ifndef TETRISH_CMDLINE
#define TETRISH_CMDLINE

#include <stddef.h>

typedef enum CmdlineResult {
    CMDLINE_OK = 0,
    CMDLINE_ERR_UNTERMINATED = -1,
    CMDLINE_ERR_NOMEM = -2,
} CmdlineResult;

/*!
    @brief Split a command line into args, POSIX-shell-style.

    Args are separated by whitespace; adjacent quoted/unquoted pieces join
    into one arg. `'...'` and `"..."` keep their content literally (each may
    contain the other's quote character). `$'...'` decodes C-style escapes
    (`\n`, `\r`, `\t`, `\a`, `\b`, `\f`, `\v`, `\\`, `\'`, `\"`; unknown
    escapes are kept verbatim). Outside quotes, `\` preserves the next
    character literally, `\` before a newline is line continuation, and a
    trailing `\` is kept as-is. No expansion is performed: `$` not followed
    by `'` is an ordinary character.

    @param out_args set to a malloc'd NULL-terminated array
    (execvp-compatible) of malloc'd strings; the caller frees each string and
    then the array. Set to NULL on error.
    @param out_len set to the number of args parsed.
*/
CmdlineResult cmdline_parse(const char* content, char*** out_args, size_t* out_len);

#endif
