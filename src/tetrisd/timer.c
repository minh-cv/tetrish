#include "timer.h"

#include <errno.h>
#include <limits.h>
#include <sys/timerfd.h>
#include <unistd.h>

int periodic_timer_create(uint64_t interval_ms) {
    if (interval_ms == 0 || interval_ms > (uint64_t)LLONG_MAX / 1000000u) {
        errno = EINVAL;
        return -1;
    }
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd == -1) {
        return -1;
    }
    const uint64_t interval_ns = interval_ms * 1000000u;
    const struct itimerspec timer = {
        .it_interval = {
            .tv_sec = (time_t)(interval_ns / 1000000000u),
            .tv_nsec = (long)(interval_ns % 1000000000u),
        },
        .it_value = {
            .tv_sec = (time_t)(interval_ns / 1000000000u),
            .tv_nsec = (long)(interval_ns % 1000000000u),
        },
    };
    if (timerfd_settime(fd, 0, &timer, NULL) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

uint64_t periodic_timer_drain(int fd) {
    uint64_t total = 0;
    for (;;) {
        uint64_t expirations = 0;
        const ssize_t received = read(fd, &expirations, sizeof(expirations));
        if (received == (ssize_t)sizeof(expirations)) {
            total += expirations;
            continue;
        }
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return total;
        }
        return 0;
    }
}
