#include "libs/shell.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>

static volatile sig_atomic_t has_child_pid = 0;
static volatile sig_atomic_t child_pid;

static void sigint_handler(int sig) {
    (void)sig;
    if (has_child_pid == 0) return;
    kill((pid_t)child_pid, SIGINT);
}

int body(FILE* input) {
    int child_status;
    int last_status = 0;
    pid_t pid;

    while (true) {
        if (input == stdin) type_prompt();

        char** cmd;
        size_t argc;
        ReadCommandResult read_result = read_command_from_file(input, &cmd, &argc);
        if (read_result == READ_COMMAND_END) {
            break;
        }
        if (read_result == READ_COMMAND_INVALID) {
            fprintf(stderr, "tetrish: cannot parse command line\n");
            last_status = 2;
            continue;
        }
        if (argc == 0) {
            free_command(cmd);
            continue;
        }

        if (input != stdin) {
            char* equals = strchr(cmd[0], '=');
            if (equals != NULL && equals != cmd[0]) {
                last_status = set_env_var((char*[]){NULL, cmd[0], NULL});
                free_command(cmd);
                continue;
            }
        }

        size_t builtin_idx = get_builtin_command_index(cmd[0]);
        if (builtin_idx != SIZE_MAX) {
            last_status = execute_builtin_command(cmd, builtin_idx);
            free_command(cmd);
            continue;
        }

        sigset_t block_int, prev_mask;
        sigemptyset(&block_int);
        sigaddset(&block_int, SIGINT);
        sigprocmask(SIG_BLOCK, &block_int, &prev_mask);

        pid = fork();
        if (pid == 0) {
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            execvp(cmd[0], cmd);
            // If execv returns, command execution has failed
            // perror(cmd[0]);
            fprintf(stderr, "%s: command not found\n", cmd[0]);
            _exit(1);
        }
        else if (pid < 0) {
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            fprintf(stderr, "Cannot spawn task\n");
            last_status = 1;
        }
        else {
            child_pid = pid;
            has_child_pid = 1;
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);

            pid_t rc;
            do {
                rc = waitpid(pid, &child_status, WUNTRACED);
            } while (rc < 0 && errno == EINTR);

            has_child_pid = 0;
            if (rc < 0) {
                last_status = 1;
            } else if (WIFSTOPPED(child_status)) {
                fprintf(stderr, "tetrish: [%d] stopped\n", (int)pid);
                last_status = 128 + WSTOPSIG(child_status);
            } else if (WIFSIGNALED(child_status)) {
                last_status = 128 + WTERMSIG(child_status);
            } else {
                last_status = WEXITSTATUS(child_status);
            }
        }
        free_command(cmd);
    }

    return last_status;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("tetrish: sigaction");
        return 1;
    }

    clear();
    FILE* shellrc = fopen(".tetrishrc", "r");
    if (shellrc != NULL) {
        body(shellrc);
        fclose(shellrc);
    }

    return body(stdin);
}