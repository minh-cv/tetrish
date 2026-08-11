#include "logger_layer.h"
#include "logger.h"
#include "socket.h"
#include "wire.h"
#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/*
    The process log handler takes no context parameter, so the single instance
    is reachable through this pointer. LoggerData_init asserts there is only
    one.
*/
static LoggerData* g_logger = NULL;

// bounded per-tick drain, so a burst of logging cannot starve player I/O
#define LOGGER_MAX_FRAMES_PER_TICK 32

// best-effort shutdown flush budget
#define LOGGER_FLUSH_ATTEMPTS 10
#define LOGGER_FLUSH_TIMEOUT_MS 20

static void writer_reset(Writer* writer) {
    writer_init(writer);
    writer->max_frames_allowed = LOGGER_MAX_FRAMES_PER_TICK;
}

/*
    The process log handler. Takes ownership of `string` on every path, and
    never logs: a record produced while reporting a dropped record would
    recurse for as long as the queue stays full.
*/
static int LoggerData_enqueue(char* string) {
    LoggerData* const data = g_logger;
    if (string == NULL) {
        return -1;
    }
    if (data == NULL) {
        free(string);
        return -1;
    }

    const size_t length = strlen(string);
    if (length == 0 || length > FRAME_MAX ||
        WriterFrameQueue_size(&data->queue) == WriterFrameQueue_capacity(&data->queue)) {
        free(string);
        data->dropped++;
        return -1;
    }

    const WriterFrame frame = { (const unsigned char*)string, length };
    const int err = WriterFrameQueue_push_back(&data->queue, &frame);
    assert(err != -1);
    (void)err;
    return 0;
}

/*
    Account for the records lost while the queue was full, once it has room
    again. The counter is snapshotted first because a failed enqueue adds this
    very record to it.
*/
static void LoggerData_report_dropped(LoggerData* data) {
    if (data->dropped == 0) {
        return;
    }
    const size_t reported = data->dropped;
    char* const string = LOGGER_MAKE_LOG(LOG_WARN, "logger", "dropped %zu records", reported);
    if (string == NULL) {
        return;
    }
    if (LoggerData_enqueue(string) == -1) {
        return;
    }
    data->dropped -= reported;
}

int LoggerData_init(LoggerData* data, struct config_var* cfg_var) {
    assert(g_logger == NULL && "one LoggerData per process");

    if (WriterFrameQueue_init(&data->queue, cfg_var->logger_capacity) == -1) {
        return -1;
    }
    writer_reset(&data->writer);
    data->fd = -1;
    data->timerfd = -1;
    data->dropped = 0;

    g_logger = data;
    logger_set_log_handler(LoggerData_enqueue);
    return 0;
}

/*
    Drain the backlog on the way out. The socket stays nonblocking, so progress
    is paced by poll() rather than by blocking sends, and the budget is fixed:
    a tetrislogd that has stopped reading must not hold up the shutdown.
*/
static void LoggerData_flush(LoggerData* data) {
    if (data->fd == -1) {
        return;
    }
    data->writer.max_frames_allowed = SIZE_MAX;

    for (int attempt = 0; attempt < LOGGER_FLUSH_ATTEMPTS; attempt++) {
        if (writer_send(&data->writer, data->fd, &data->queue) == -1) {
            return;
        }
        if (!LoggerData_wants_write(data)) {
            return;
        }
        struct pollfd pfd = { data->fd, POLLOUT, 0 };
        if (poll(&pfd, 1, LOGGER_FLUSH_TIMEOUT_MS) <= 0) {
            return;
        }
    }
}

void LoggerData_free(LoggerData* data) {
    // before anything else: the teardown below must not append to the queue
    logger_set_log_handler(logger_log_null);
    g_logger = NULL;

    LoggerData_flush(data);

    if (data->fd != -1) {
        close(data->fd);
        data->fd = -1;
    }
    if (data->timerfd != -1) {
        close(data->timerfd);
        data->timerfd = -1;
    }
    writer_free(&data->writer);
    /*
        Last resort for what the flush could not deliver: stderr is /dev/null
        under the daemon, but in the foreground it is the only way a startup
        failure, whose records never reached a socket at all, is visible.
    */
    const size_t left = WriterFrameQueue_size(&data->queue);
    for (size_t i = 0; i < left; i++) {
        const WriterFrame* frame = WriterFrameQueue_front(&data->queue);
        fwrite(frame->ptr, 1, frame->length, stderr);
        free((void*)frame->ptr);
        WriterFrameQueue_pop_front(&data->queue);
    }
    WriterFrameQueue_free(&data->queue);
}

void LoggerData_arm_timerfd(LoggerData* data, struct config_var* cfg_var) {
    assert(data->fd == -1);
    assert(data->timerfd == -1);

    const int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) {
        LOGGER_PERROR("logger", "timerfd_create");
        return;
    }

    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = (time_t)cfg_var->logger_reconnect_seconds;
    if (timerfd_settime(timerfd, 0, &spec, NULL) == -1) {
        LOGGER_PERROR("logger", "timerfd_settime");
        close(timerfd);
        return;
    }

    data->timerfd = timerfd;
}

void LoggerData_accept(LoggerData* data, struct config_var* cfg_var) {
    assert(data->fd == -1);
    assert(data->timerfd == -1);

    data->fd = prepare_logger_socket(cfg_var->log_ipc);
    if (data->fd != -1) {
        return;
    }
    LoggerData_arm_timerfd(data, cfg_var);
}

void LoggerData_close(LoggerData* data) {
    assert(data->fd != -1);

    close(data->fd);
    data->fd = -1;

    if (data->writer.state != WRITER_IDLE) {
        // the peer saw a truncated record; it cannot be resent from the middle
        data->dropped++;
    }
    writer_free(&data->writer);
    writer_reset(&data->writer);
}

void LoggerData_read_timerfd(LoggerData* data) {
    assert(data->timerfd != -1);

    uint64_t expirations;
    const ssize_t n = read(data->timerfd, &expirations, sizeof(expirations));
    if (n != (ssize_t)sizeof(expirations)) {
        LOGGER_PERROR("logger", "timerfd read");
    }
}

void LoggerData_close_timerfd(LoggerData* data) {
    assert(data->timerfd != -1);

    close(data->timerfd);
    data->timerfd = -1;
}

void LoggerData_write(LoggerData* data, WriterFrameQueue* m_write_q) {
    assert(data->fd != -1);

    LoggerData_report_dropped(data);

    if (writer_send(&data->writer, data->fd, m_write_q) == -1) {
        // no LOGGER_PERROR: the record would go to the connection that just died
        LoggerData_close(data);
    }
}

bool LoggerData_wants_write(const LoggerData* data) {
    return !WriterFrameQueue_empty(&data->queue) || data->writer.state != WRITER_IDLE;
}
