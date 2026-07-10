#ifndef SHELL_H
#define SHELL_H

#include <limits.h> // For PATH_MAX
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define BIN_PATH "./bin/"

void type_prompt();
void clear();
bool read_command(char **cmd);
size_t get_builtin_command_index(char* cmd);
void execute_builtin_command(char** cmd, size_t index);
bool read_command_from_file(char** cmd, FILE* file);
int set_env_var(char **args);

#endif