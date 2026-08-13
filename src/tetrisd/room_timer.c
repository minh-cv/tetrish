#include "room_timer.h"
#include "logger.h"
#include <errno.h>
#include <unistd.h>

#define NS_PER_S 1000000000ULL

static struct itimerspec spec_from_hz(unsigned int tick_hz) {
    const unsigned long long period_ns = NS_PER_S / tick_hz;
    const struct timespec period = {
        .tv_sec = (time_t)(period_ns / NS_PER_S),
        .tv_nsec = (long)(period_ns % NS_PER_S),
    };
    // it_value doubles as the first expiry, so the clock starts one period from
    // the call rather than waiting for a separate arm
    return (struct itimerspec){.it_interval = period, .it_value = period};
}

static bool spec_eq(const struct itimerspec* a, const struct itimerspec* b) {
    return a->it_interval.tv_sec == b->it_interval.tv_sec &&
           a->it_interval.tv_nsec == b->it_interval.tv_nsec;
}

int RoomTimer_init(RoomTimer* data, unsigned int tick_hz) {
    if (tick_hz == 0) {
        LOGGER_LOG(LOG_ERROR, "room_timer", "tick_hz of 0 would disarm the clock");
        return -1;
    }

    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd == -1) {
        LOGGER_PERROR("room_timer", "timerfd_create");
        return -1;
    }

    const struct itimerspec spec = spec_from_hz(tick_hz);
    if (timerfd_settime(fd, 0, &spec, NULL) == -1) {
        LOGGER_PERROR("room_timer", "timerfd_settime");
        close(fd);
        return -1;
    }

    data->fd = fd;
    data->tick_spec = spec;
    data->expirations = 0;
    return 0;
}

void RoomTimer_free(RoomTimer* data) {
    close(data->fd);
    data->fd = -1;
}

int RoomTimer_reconfig(RoomTimer* data, unsigned int tick_hz) {
    if (tick_hz == 0) {
        LOGGER_LOG(LOG_WARN, "room_timer", "tick_hz of 0 would disarm the clock, keeping the armed rate");
        return -1;
    }

    const struct itimerspec spec = spec_from_hz(tick_hz);
    if (spec_eq(&spec, &data->tick_spec)) {
        return 0;
    }
    if (timerfd_settime(data->fd, 0, &spec, NULL) == -1) {
        LOGGER_PERROR("room_timer", "timerfd_settime");
        return -1;
    }

    data->tick_spec = spec;
    LOGGER_LOG(LOG_INFO, "room_timer", "tick rate set to %u hz", tick_hz);
    return 0;
}

void RoomTimer_reset(RoomTimer* data) {
    data->expirations = 0;
}

int RoomTimer_read(RoomTimer* data, uint64_t* m_expirations_out) {
    *m_expirations_out = 0;

    uint64_t count;
    ssize_t n;
    do {
        n = read(data->fd, &count, sizeof(count));
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        LOGGER_PERROR("room_timer", "read");
        return -1;
    }
    // a timerfd read is all-or-nothing; a short one means the fd is not the
    // timer anymore, which no retry fixes
    if (n != (ssize_t)sizeof(count)) {
        LOGGER_LOG(LOG_ERROR, "room_timer", "short read of %zd bytes", n);
        return -1;
    }

    *m_expirations_out = count;
    return 0;
}
