#include "libs/shell.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void run_external(char** argv) {
    const pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "%s: command not found\n", argv[0]);
        _exit(127);
    }

    int child_status;
    if (waitpid(pid, &child_status, WUNTRACED) == -1) {
        perror("waitpid");
        return;
    }
    if (!WIFEXITED(child_status)) {
        printf("tetrish: child terminated abnormally\n");
        return;
    }
    const int exit_status = WEXITSTATUS(child_status);
    if (exit_status != 0) {
        printf("tetrish: child exited with status %d\n", exit_status);
    }
}

/*
    One REPL for both the startup script and the terminal, so a line behaves
    the same however it arrives.
*/
static void body(FILE* input) {
    for (;;) {
        if (input == stdin) {
            type_prompt();
        }

        char* argv[SHELL_ARGV_SIZE] = {0};
        size_t argc = 0;
        const ShellReadResult result = read_command_from_file(argv, &argc, input);

        if (result == SHELL_READ_EOF) {
            return;
        }
        if (result == SHELL_READ_EMPTY) {
            continue;
        }

        /*
            The .tetrishrc format lets a bare `KEY=value` line set a variable,
            which the interactive prompt deliberately does not: at the prompt
            that spelling is far more likely to be a mistyped command.
        */
        if (input != stdin && strchr(argv[0], '=') != NULL &&
            get_builtin_command_index(argv[0]) == SIZE_MAX) {
            char* pair[] = {NULL, argv[0], NULL};
            set_env_var(pair);
            cmdline_free(argv, argc);
            continue;
        }

        const size_t builtin_idx = get_builtin_command_index(argv[0]);
        if (builtin_idx != SIZE_MAX) {
            execute_builtin_command(argv, builtin_idx);
        }
        else {
            run_external(argv);
        }

        cmdline_free(argv, argc);
    }
}

int main(void) {
    clear();

    FILE* const shellrc = fopen(".tetrishrc", "r");
    if (shellrc != NULL) {
        body(shellrc);
        fclose(shellrc);
    }

    body(stdin);
    return 0;
}
