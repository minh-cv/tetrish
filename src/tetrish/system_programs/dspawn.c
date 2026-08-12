#include <fcntl.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    DAEMONIZE_CHILD,  // caller is the daemon and should carry on
    DAEMONIZE_PARENT, // caller is an intermediate process and should exit
    DAEMONIZE_ERROR,
} DaemonizeResult;

static DaemonizeResult incantation() {
    int pid = fork();
    if (pid < 0) {
        perror("dspawn: fork");
        return DAEMONIZE_ERROR;
    }
    if (pid > 0) {
        return DAEMONIZE_PARENT;
    }

    if (setsid() == -1) {
        perror("dspawn: setsid");
        return DAEMONIZE_ERROR;
    }
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    int daemon_pid = fork();
    if (daemon_pid < 0) {
        perror("dspawn: fork");
        return DAEMONIZE_ERROR;
    }
    if (daemon_pid > 0) {
        return DAEMONIZE_PARENT;
    }

    umask(0);
    if (chdir("/") == -1) {
        perror("dspawn: chdir");
        return DAEMONIZE_ERROR;
    }

    for (int x = (int)sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    /*
    * Attach file descriptors 0, 1, and 2 to /dev/null. */
    if (open("/dev/null", O_RDWR) == -1 || dup(0) == -1 || dup(0) == -1) {
        return DAEMONIZE_ERROR;
    }

    return DAEMONIZE_CHILD;
}

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
    case DAEMONIZE_PARENT:
        _exit(0);
    case DAEMONIZE_ERROR:
        return 1;
    case DAEMONIZE_CHILD:
        break;
    }

    return daemon_work(log_out);
}
