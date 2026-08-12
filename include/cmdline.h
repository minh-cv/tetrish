#ifndef TETRISH_CMDLINE
#define TETRISH_CMDLINE

#include <stddef.h>

/*!
    @brief Split a command line into args, shell-style.

    Args are separated by whitespace. `'...'` keeps its content literally;
    `"..."` and bare text process `\` escapes (`\n`, `\r`, `\t`, otherwise
    the escaped char itself). Adjacent quoted/unquoted pieces join into one
    arg.

    @param out_len set to the number of args parsed.
    @return a malloc'd NULL-terminated array (execvp-compatible) of `*out_len`
    malloc'd strings; the caller frees each string and then the array. NULL on
    unterminated quote or allocation failure.
*/
char** cmdline_parse(const char* content, size_t* out_len);

#endif
