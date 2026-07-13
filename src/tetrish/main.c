#include "libs/shell.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

volatile sig_atomic_t has_child_pid = 0;
pid_t child_pid;

static void sigint_handler(int _) {
    if (has_child_pid == 0) return;
    kill(child_pid, SIGINT);
}

static void free_cmd(char** cmd) {
    for (int i = 0; cmd[i] != NULL && i < MAX_ARGS; i++) {
        free(cmd[i]);
    }
}

void body(FILE* input) {
    // Define an array to hold the command and its arguments
    int child_status;
    pid_t pid;
    
    while (true) {
        char *cmd[MAX_ARGS] = {0};
        if (input == stdin) type_prompt();     // Display the prompt
        bool read_command_result = read_command_from_file(cmd, input); // Read a command from the user
        if (cmd[0] == NULL) {
            free_cmd(cmd);
            if (!read_command_result) {
                break;
            }
            continue;
        }

        if (input != stdin) {
            if (strncmp(cmd[0], "PATH=", 5) == 0) {
                set_env_var((char*[]){NULL, cmd[0]});
                free_cmd(cmd);
                continue;
            }
        }

        size_t builtin_idx = get_builtin_command_index(cmd[0]);
        if (builtin_idx != SIZE_MAX) {
            execute_builtin_command(cmd, builtin_idx);
            free_cmd(cmd);
            continue;
        }

        pid = fork();
        if (pid == 0) {
            execvp(cmd[0], cmd);
            // If execv returns, command execution has failed
            // perror(cmd[0]);
            fprintf(stderr, "%s: command not found\n", cmd[0]);
            _exit(1);
        }
        else if (pid < 0) {
            fprintf(stderr, "Cannot spawn task\n");
        }
        else {
            child_pid = pid;
            has_child_pid = 1;
            while (true) {
                int rc = waitpid(pid, &child_status, WUNTRACED);
                if (rc == 0) break;
                if (rc == -1) {
                    if (errno != EINTR) break;
                }
            }
            has_child_pid = 0;
        }
        free_cmd(cmd);
        if (!read_command_result) {
            break;
        }
    }
}

// The main function where the shell's execution begins
int main(void) {
    signal(SIGINT, sigint_handler);
    clear();
    FILE* shellrc = fopen(".tetrishrc", "r");
    if (shellrc != NULL) {
        body(shellrc);
        fclose(shellrc);
    }
    body(stdin);
}