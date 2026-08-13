// This code was written by ChatGPT4
// Modify it for your own usage to implement features for PA1 (or completely
// rewrite it) Include the shell header file for necessary constants and
// function declarations
#include "libs/shell.h"
#include "cmdline.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int shell_cd(char** args);
static int shell_help(char** args);
static int shell_exit(char** args);
static int shell_usage(char** args);
static int list_env(char** args);
static int unset_env_var(char** args);

typedef struct {
    const char* name;
    int (*func)(char** args);
    const char* usage;
} BuiltinCommand;

static const BuiltinCommand builtin_commands[] = {
    {"cd", shell_cd,
     "cd [<directory>]\nChange the current working directory of the shell. Defaults to $HOME.\n"},
    {"help", shell_help,
     "help\nList all builtin commands.\n"},
    {"exit", shell_exit,
     "exit [<status>]\nTerminate the shell, exiting with <status> if given.\n"},
    {"usage", shell_usage,
     "usage <cmd>\nPrint how to use a command.\n"},
    {"env", list_env,
     "env\nList all environment variables.\n"},
    {"setenv", set_env_var,
     "setenv <env-name>=<value>\nSet an environment variable.\n"},
    {"unsetenv", unset_env_var,
     "unsetenv <env-name>\nUnset an environment variable.\n"},
};

static const size_t builtin_commands_count =
    sizeof builtin_commands / sizeof *builtin_commands;

static int shell_cd(char **args) {
    const char* path = args[1];
    if (path == NULL) {
        path = getenv("HOME");
        if (path == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            return 1;
        }
    }
    if (chdir(path) != 0) {
        perror("cd");
        return 1;
    }
    return 0;
}

static int shell_help(char **args) {
    (void)args;
    printf("The following commands are implemented within the shell:\n");
    for (size_t i = 0; i < builtin_commands_count; i++) {
        printf("    %s\n", builtin_commands[i].name);
    }
    return 0;
}

static int shell_exit(char **args) {
    int status = 0;
    if (args[1] != NULL) {
        char* end;
        errno = 0;
        long value = strtol(args[1], &end, 10);
        if (*args[1] == '\0' || *end != '\0' || errno == ERANGE ||
            value < 0 || value > 255) {
            fprintf(stderr, "exit: numeric argument required\n");
            status = 2;
        } else {
            status = (int)value;
        }
    }
    exit(status);
}

static int shell_usage(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: usage <command>.\n");
        return 1;
    }

    size_t index = get_builtin_command_index(args[1]);
    if (index == SIZE_MAX) {
        fprintf(stderr, "Non-builtin command not supported.\n");
        return 2;
    }
    fputs(builtin_commands[index].usage, stdout);
    return 0;
}

static int list_env(char **args) {
    (void)args;

    extern char** environ;
    char** env = environ;

    while (*env != NULL) {
        printf("%s\n", *env);
        env++;
    }

    return 0;
}

int set_env_var(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: setenv <env-name>=<value>\n");
        return 1;
    }
    char* eq_at = strchr(args[1], '=');
    if (eq_at == NULL ||
        eq_at == args[1] // no name
    ) {
        fprintf(stderr, "Usage: setenv <env-name>=<value>\n");
        return 1;
    }
    *eq_at = '\0';
    int return_val = setenv(args[1], eq_at + 1, 1);
    *eq_at = '=';
    if (return_val != 0) {
        perror("setenv");
        return 1;
    }
    return return_val;
}

static int unset_env_var(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: unsetenv <env-name>\n");
        return 1;
    }
    int retval = unsetenv(args[1]);
    if (retval != 0) {
        perror("unsetenv");
        return 1;
    }
    return retval;
}

size_t get_builtin_command_index(const char *cmd) {
    for (size_t i = 0; i < builtin_commands_count; i++) {
        if (strcmp(cmd, builtin_commands[i].name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

int execute_builtin_command(char **cmd, size_t index) {
    assert(index < builtin_commands_count);
    return builtin_commands[index].func(cmd);
}

ReadCommandResult read_command_from_file(FILE* file, char*** out_argv, size_t* out_argc) {
    *out_argv = NULL;
    *out_argc = 0;

    char* line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) {
        free(line);
        if (ferror(file)) {
            perror("tetrish: read");
        }
        return READ_COMMAND_END;
    }

    // a line whose first non-blank character is `#` is a comment; dropping it
    // here leaves an empty command, which the caller already skips.
    char* first = line;
    while (isspace((unsigned char)*first)) first++;
    if (*first == '#') *first = '\0';

    char** argv;
    CmdlineResult parse_result = cmdline_parse(line, &argv, out_argc);
    free(line);
    if (parse_result != CMDLINE_OK) {
        return READ_COMMAND_INVALID;
    }

    *out_argv = argv;
    return READ_COMMAND_OK;
}

void free_command(char** argv) {
    if (argv == NULL) {
        return;
    }
    for (char** arg = argv; *arg != NULL; arg++) {
        free(*arg);
    }
    free(argv);
}

void type_prompt() {
  fputs(TETRISH_PROMPT, stdout);
  fflush(stdout);
}

void clear() {
    fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
    fflush(stdout);
}
