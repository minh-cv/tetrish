#include <fcntl.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int incantation() {
    int pid = fork();
    if (pid < 0) {
        perror("dspawn");
        return 1;
    }
    if (pid > 0) {
        return 1;
    }
    setsid();
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    
    int daemon_pid = fork();
    if (daemon_pid < 0) {
        perror("dspawn");
        return 1;
    }
    if (daemon_pid > 0) {
        return 1;
    }
    umask(0);
    chdir("/");
    for (int x = (int)sysconf(_SC_OPEN_MAX); x>=0; x--) {
        close(x);
    }

    /*
    * Attach file descriptors 0, 1, and 2 to /dev/null. */
    open("/dev/null", O_RDWR);
    dup(0);
    dup(0);


    return 0;
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
    if (written_char >= PATH_MAX) {
        fprintf(stderr, "Error: path too long");
    }
    if (incantation()) return 0;
    daemon_work(log_out);
}