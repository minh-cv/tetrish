#ifndef SHELL_H
#define SHELL_H

#include "cmdline.h"
#include <limits.h> // For PATH_MAX
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/*
    The argument vector handed to execvp needs a NULL terminator past the last
    token, which is why this is one longer than the tokenizer's limit. Writing
    that terminator into the last usable slot was the original overflow.
*/
#define SHELL_ARGV_SIZE (CMDLINE_MAX_ARGS + 1)

typedef enum {
    SHELL_READ_OK,      // argv holds argc tokens plus a NULL terminator
    SHELL_READ_EMPTY,   // nothing to run: a blank line, or one already reported
    SHELL_READ_EOF,     // the input is exhausted
} ShellReadResult;

void type_prompt(void);
void clear(void);

size_t get_builtin_command_index(const char* cmd);
void execute_builtin_command(char** cmd, size_t index);
int set_env_var(char** args);

/*!
    @brief read one line from @p file and split it into @p argv

    Lines are read whole, however long they are, and quoting follows
    cmdline_split: `'...'` is literal and `"..."` processes escapes. A line
    that cannot be split is reported and skipped rather than run in part, and
    is never a reason to end the shell — the spec's "no crashes on bad input"
    is about exactly this path.

    @post on SHELL_READ_OK, @p argv holds @p out_argc heap tokens followed by
          NULL, and the caller frees them with cmdline_free
*/
ShellReadResult read_command_from_file(char* argv[SHELL_ARGV_SIZE], size_t* out_argc, FILE* file);

#endif
