#include "daemon.h"
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int daemon_work(const char* log_out) {
    FILE* logger = fopen(log_out, "a");
    if (logger == NULL) return 1;

    fprintf(logger, "Daemon process running with PID: %d, PPID: %d, opening logfile with FD %d\n", getpid(), getppid(), fileno(logger));

    for (int i = 0; i < 10; i++) {
        fprintf(logger, "PID %d writing line %d\n", getpid(), i + 1);
        fflush(logger);
        sleep(2);
    }

    fclose(logger);
    return 0;
}

int main() {
    char log_out[PATH_MAX];
    char* project_dir = getenv("PROJECT_DIR");
    if (project_dir == NULL) {
        fprintf(stderr, "Error: PROJECT_DIR is not set.\n");
        return 1;
    }
    int written_char = snprintf(log_out, sizeof log_out, "%s/dspawn.log", project_dir);
    if (written_char < 0 || written_char >= (int)sizeof log_out) {
        fprintf(stderr, "Error: log path too long.\n");
        return 1;
    }

    switch (incantation()) {
    case 0:
        return 0;
    case -1:
        perror("incantation");
        return EXIT_FAILURE;
    case 1:
        break;
    default:
        return EXIT_FAILURE;
    }

    return daemon_work(log_out);
}
