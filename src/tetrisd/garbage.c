#include "garbage.h"
#include "dtor.h"
#include "logger.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static DTOR_WRAPPER_DEFINE(free)

static void garbage_mq_dispose(GarbageData* data) {
    mq_close(data->mq);
    mq_unlink(data->mq_name);
    data->mq = (mqd_t)-1;
}
static DTOR_WRAPPER_DEFINE(garbage_mq_dispose)

int GarbageData_init(GarbageData* data, const struct config_var* cfg) {
    DTOR_DEFINE(errdtor, 3);
    DTOR_DEFINE(dtor, 1);

    // stale queue from a previous run; ENOENT is the normal case
    if (mq_unlink(cfg->garbage_ipc) == -1 && errno != ENOENT) {
        LOGGER_PERROR("garbage", "mq_unlink");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    struct mq_attr requested;
    memset(&requested, 0, sizeof(requested));
    requested.mq_maxmsg = (long)cfg->garbage_queue_depth;
    requested.mq_msgsize = sizeof(GarbageEvent);

    // incantation() runs umask(0), so scope a restrictive mask over the
    // create: the queue's 0600 mode is who may inject garbage
    const mode_t old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    mqd_t mq = mq_open(cfg->garbage_ipc, O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK,
                       S_IRUSR | S_IWUSR, &requested);
    if (mq == (mqd_t)-1 && errno == EINVAL) {
        // the depth exceeds the kernel's cap; the kernel defaults still work
        LOGGER_LOG(LOG_WARN, "garbage", "queue depth %u rejected by the kernel, using its defaults",
                   cfg->garbage_queue_depth);
        mq = mq_open(cfg->garbage_ipc, O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK,
                     S_IRUSR | S_IWUSR, NULL);
    }
    umask(old_umask);
    if (mq == (mqd_t)-1) {
        LOGGER_PERROR("garbage", "mq_open");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    data->mq = mq;
    data->mq_name = cfg->garbage_ipc;
    DTOR_INSERT(errdtor, garbage_mq_dispose, data);

    struct mq_attr actual;
    if (mq_getattr(mq, &actual) == -1) {
        LOGGER_PERROR("garbage", "mq_getattr");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    assert(actual.mq_msgsize >= (long)sizeof(GarbageEvent) &&
           "no fallback path shrinks the message size below the event");
    data->mq_msgsize = actual.mq_msgsize;

    data->recv_buf = malloc((size_t)actual.mq_msgsize);
    if (data->recv_buf == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, data->recv_buf);

    if (Vec_GarbageEvent_init(&data->received, (size_t)actual.mq_maxmsg) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    data->seq = 0;
    data->sent = 0;
    data->dropped_full = 0;
    data->dropped_bad_event = 0;
    data->reported_dropped_full = 0;
    LOGGER_LOG(LOG_INFO, "garbage", "queue %s open, depth %ld",
               data->mq_name, actual.mq_maxmsg);
    DTOR_RETURN(dtor, 0);
}

void GarbageData_free(GarbageData* data) {
    Vec_GarbageEvent_free(&data->received);
    free(data->recv_buf);
    data->recv_buf = NULL;
    if (data->mq != (mqd_t)-1) {
        if (mq_close(data->mq) == -1) {
            LOGGER_PERROR("garbage", "mq_close");
        }
        if (mq_unlink(data->mq_name) == -1 && errno != ENOENT) {
            LOGGER_PERROR("garbage", "mq_unlink");
        }
        data->mq = (mqd_t)-1;
    }
}

/*
    Account for the events lost while the queue was full, in one line. The
    shape of LoggerData_report_dropped: per-event lines from a full queue
    would flood the very logger whose never-block rule this mirrors.
*/
static void report_dropped_full(GarbageData* data) {
    if (data->dropped_full == data->reported_dropped_full) {
        return;
    }
    LOGGER_LOG(LOG_WARN, "garbage", "dropped %llu events on a full queue",
               (unsigned long long)(data->dropped_full - data->reported_dropped_full));
    data->reported_dropped_full = data->dropped_full;
}

void GarbageData_send(GarbageData* data, const Vec_GarbageEvent* events) {
    for (size_t i = 0; i < Vec_GarbageEvent_size(events); i++) {
        GarbageEvent event = *Vec_GarbageEvent_at(events, i);
        event.seq = data->seq++;
        if (mq_send(data->mq, (const char*)&event, sizeof(event), 0) == -1) {
            if (errno != EAGAIN) {
                LOGGER_PERROR("garbage", "mq_send");
            }
            data->dropped_full++;
            continue;
        }
        data->sent++;
    }
    report_dropped_full(data);
}

int GarbageData_receive(GarbageData* data, Vec_GarbageEvent* m_received) {
    while (Vec_GarbageEvent_size(m_received) < Vec_GarbageEvent_capacity(m_received)) {
        const ssize_t n = mq_receive(data->mq, (char*)data->recv_buf,
                                     (size_t)data->mq_msgsize, NULL);
        if (n == -1) {
            if (errno == EAGAIN) {
                return 0;
            }
            LOGGER_PERROR("garbage", "mq_receive");
            return -1;
        }

        if ((size_t)n != sizeof(GarbageEvent)) {
            data->dropped_bad_event++;
            LOGGER_LOG(LOG_WARN, "garbage", "dropped a message of %zd bytes", n);
            continue;
        }
        GarbageEvent event;
        memcpy(&event, data->recv_buf, sizeof(event));
        if (event.version != GARBAGE_EVENT_VERSION) {
            data->dropped_bad_event++;
            LOGGER_LOG(LOG_WARN, "garbage", "dropped an event of version %u", event.version);
            continue;
        }

        const int err = Vec_GarbageEvent_push_back(m_received, &event);
        assert(err != -1 && "the loop stops at capacity");
        (void)err;
    }
    return 0;
}

void GarbageData_reset(GarbageData* data) {
    Vec_GarbageEvent_reset(&data->received);
}
