// This code was written by ChatGPT4
// Modify it for your own usage to implement features for PA1 (or completely
// rewrite it) Include the shell header file for necessary constants and
// function declarations
#include "libs/shell.h"
#include "cmdline.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int shell_cd(char **args) {
    char* path = args[1] == NULL ? getenv("HOME") : args[1];
    if (chdir(path) != 0) {
        perror("cd");
        return 1;
    }
    return 0;
}

static int shell_help(char **args) {
    (void)args;
    printf(
"The following commands are implemented within the shell:\n\
    cd\n\
    help\n\
    exit\n\
    usage\n\
    env\n\
    setenv\n\
    unsetenv\n"
    );

    return 0;
}

static int shell_exit(char **args) {
    (void)args;
    exit(0);
}

static int shell_usage(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "Usage: usage <command>.\n");
        return 1;
    }

    size_t cmd_idx = get_builtin_command_index(args[1]);
    switch (cmd_idx) {
    case 0: // cd
        printf("cd [<directory>]\nChange the current working directory of the shell. Defaults to $HOME.\n");
        break;
    case 1: // help
        printf("help\nList all builtin commands.\n");
        break;
    case 2: // exit
        printf("exit\nTerminate the shell.\n");
        break;
    case 3: // usage
        printf("usage <cmd>\nPrint how to use a command.\n");
        break;
    case 4: // env
        printf("env\nList all environment variables.\n");
        break;
    case 5: // setenv
        printf("setenv <env-name>=<value>\nSet an environment variable.\n");
        break;
    case 6: // unsetenv
        printf("unsetenv <env-name>\nUnset an environment variable.\n");
        break;
    default:
        fprintf(stderr, "Non-builtin command not supported.\n");
        return 2;
    }
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

static const char *builtin_commands[] = {
    "cd",   // Changes the current directory of the shell to the specified path.
            // If no path is given, it defaults to the user's home directory.
    "help", //  List all builtin commands in the shell
    "exit", // Exits the shell
    "usage",  // Provides a brief usage guide for the shell and its built-in
              // command
    "env",    // Lists all the environment variables currently set in the shell
    "setenv", // Sets or modifies an environment variable for this shell session
    "unsetenv" // Removes an environment variable from the shell
};

static int (*builtin_command_func[])(char **) = {
    &shell_cd,     // builtin_command_func[0]: cd
    &shell_help,   // builtin_command_func[1]: help
    &shell_exit,   // builtin_command_func[2]: exit
    &shell_usage,  // builtin_command_func[3]: usage
    &list_env,     // builtin_command_func[4]: env
    &set_env_var,  // builtin_command_func[5]: setenv
    &unset_env_var // builtin_command_func[6]: unsetenv
};

size_t get_builtin_command_index(const char *cmd) {
    for (size_t i = 0; i < sizeof(builtin_commands)/sizeof(const char*); i++) {
        if (strcmp(cmd, builtin_commands[i]) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

void execute_builtin_command(char **cmd, size_t index) {
    builtin_command_func[index](cmd);
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

    char** argv = cmdline_parse(line, out_argc);
    free(line);
    if (argv == NULL) {
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
  printf("$$ ");
  fflush(stdout);
}

void clear() {
    fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
    fflush(stdout);
}
