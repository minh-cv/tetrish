#ifndef SHELL_H
#define SHELL_H

#include <stddef.h>
#include <stdio.h>

typedef enum {
    READ_COMMAND_OK,      // *out_argv holds a (possibly empty) parsed command
    READ_COMMAND_END,     // input is exhausted or unreadable
    READ_COMMAND_INVALID, // this line did not parse; later lines still might
} ReadCommandResult;

void type_prompt();
void clear();
size_t get_builtin_command_index(const char* cmd);
void execute_builtin_command(char** cmd, size_t index);

/*!
    @brief Read one line from @p file and split it with `cmdline_parse`.

    @post on READ_COMMAND_OK @p out_argv is a malloc'd NULL-terminated argv of
    @p out_argc args, released with `free_command`; it is NULL otherwise.
*/
ReadCommandResult read_command_from_file(FILE* file, char*** out_argv, size_t* out_argc);
void free_command(char** argv);
int set_env_var(char** args);

#endif
