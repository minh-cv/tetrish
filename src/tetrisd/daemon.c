#include "daemon.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef TETRISH_TETRISD_NO_DAEMON
int incantation() {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    else if (pid > 0) {
        return 0;
    }

    assert(setsid() != -1);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    else if (pid > 0) {
        return 0;
    }

    umask(0);

    if (chdir("/") == -1) {
        return -1;
    }

    for (int fd = 0; fd < sysconf(_SC_OPEN_MAX); fd++) {
        close(fd);
    }

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd == -1) {
        return -1;
    }

    if (dup2(null_fd, STDOUT_FILENO) == -1 || dup2(null_fd, STDERR_FILENO) == -1 || dup2(null_fd, STDIN_FILENO) == -1) {
        return -1;
    }

    return 1;
}
#else
int incantation() {
    return 1;
}
#endif