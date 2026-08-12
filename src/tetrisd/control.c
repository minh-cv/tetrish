#include "control.h"
#include "dtor.h"
#include "htttp.h"
#include "logger.h"
#include "sig.h"
#include "socket.h"
#include "wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// one request plus one pipelined extra the reader may complete before the
// close lands; only the front frame is ever served
#define CONTROL_QUEUE_CAP 2u

static void close_ptr(int* fd) {
    close(*fd);
}

static void unlink_path(char* path) {
    if (unlink(path) == -1 && errno != ENOENT) {
        LOGGER_PERROR("control", "unlink");
    }
}

static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(unlink_path)
static DTOR_WRAPPER_DEFINE(Vec_Fd_free)

/*!
    @see writer_queue_drain (player_io.c) — intentional duplicate
*/
static void writer_queue_drain(WriterFrameQueue* q) {
    const size_t count = WriterFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        free((void*)WriterFrameQueue_front(q)->ptr);
        WriterFrameQueue_pop_front(q);
    }
}

/*!
    @see reader_queue_drain (player_io.c) — intentional duplicate
*/
static void reader_queue_drain(ReaderFrameQueue* q) {
    const size_t count = ReaderFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const ReaderFrame* frame = ReaderFrameQueue_front(q);
        if (frame->status == READER_FRAME_OK) {
            free(frame->content.ptr);
        }
        ReaderFrameQueue_pop_front(q);
    }
}

static ControlConn* conn_slot_at(ControlData* data, Fd fd) {
    for (size_t i = 0; i < CONTROL_MAX_CONNS; i++) {
        if (data->conns[i].fd == fd) {
            return &data->conns[i];
        }
    }
    return NULL;
}

static int conn_slot_init(ControlConn* conn, Fd fd) {
    if (ReaderFrameQueue_init(&conn->read_q, CONTROL_QUEUE_CAP) == -1) {
        return -1;
    }
    if (WriterFrameQueue_init(&conn->write_q, CONTROL_QUEUE_CAP) == -1) {
        ReaderFrameQueue_free(&conn->read_q);
        return -1;
    }
    reader_init(&conn->reader);
    writer_init(&conn->writer);
    conn->state = CONTROL_CONN_READING;
    conn->shutdown_on_teardown = false;
    conn->fd = fd;
    return 0;
}

static void conn_slot_free(ControlConn* conn) {
    reader_free(&conn->reader);
    writer_free(&conn->writer);
    reader_queue_drain(&conn->read_q);
    ReaderFrameQueue_free(&conn->read_q);
    writer_queue_drain(&conn->write_q);
    WriterFrameQueue_free(&conn->write_q);
    conn->fd = -1;
}

int Control_init(ControlData* data, const char* control_ipc) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(control_ipc) >= sizeof(addr.sun_path)) {
        LOGGER_LOG(LOG_ERROR, "control", "control_ipc path too long");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    strcpy(addr.sun_path, control_ipc);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        LOGGER_PERROR("control", "socket");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, close_ptr, &listen_fd);

    if (set_nonblocking(listen_fd) == -1) {
        LOGGER_PERROR("control", "set_nonblocking");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    // stale socket file from a previous run; ENOENT is the normal case
    if (unlink(control_ipc) == -1 && errno != ENOENT) {
        LOGGER_PERROR("control", "unlink");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    // incantation() runs umask(0), so scope a restrictive mask over bind: the
    // socket file's 0600 mode is the control plane's entire auth boundary
    const mode_t old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    const int bind_err = bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    umask(old_umask);
    if (bind_err == -1) {
        LOGGER_PERROR("control", "bind");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, unlink_path, (char*)control_ipc);

    // small backlog: the busy policy drains it immediately, and admin clients
    // arriving faster than it empties deserve ECONNREFUSED over queueing
    if (listen(listen_fd, 4) == -1) {
        LOGGER_PERROR("control", "listen");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    if (Vec_Fd_init(&data->accepted, CONTROL_MAX_CONNS) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Vec_Fd_free, &data->accepted);

    if (Vec_Fd_init(&data->conns_reading, CONTROL_MAX_CONNS) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Vec_Fd_free, &data->conns_reading);

    if (Vec_Fd_init(&data->conns_writing, CONTROL_MAX_CONNS) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Vec_Fd_free, &data->conns_writing);

    if (Vec_WriterQueueStatusEntry_init(&data->write_qs_status, CONTROL_MAX_CONNS) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    for (size_t i = 0; i < CONTROL_MAX_CONNS; i++) {
        data->conns[i].fd = -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &data->start_time);
    data->listen_fd = listen_fd;
    data->ipc_path = control_ipc;
    DTOR_RETURN(dtor, 0);
}

void Control_free(ControlData* data) {
    for (size_t i = 0; i < CONTROL_MAX_CONNS; i++) {
        if (data->conns[i].fd != -1) {
            conn_slot_free(&data->conns[i]);
        }
    }
    close(data->listen_fd);
    data->listen_fd = -1;
    unlink_path((char*)data->ipc_path);
    Vec_WriterQueueStatusEntry_free(&data->write_qs_status);
    Vec_Fd_free(&data->conns_writing);
    Vec_Fd_free(&data->conns_reading);
    Vec_Fd_free(&data->accepted);
}

void Control_reset(ControlData* data) {
    for (size_t i = 0; i < CONTROL_MAX_CONNS; i++) {
        if (data->conns[i].fd != -1) {
            reader_queue_drain(&data->conns[i].read_q);
        }
    }
    Vec_Fd_reset(&data->accepted);
    Vec_Fd_reset(&data->conns_reading);
    Vec_Fd_reset(&data->conns_writing);
    Vec_WriterQueueStatusEntry_reset(&data->write_qs_status);
}

void Control_accept(ControlData* data, size_t fd_capacity, Vec_Fd* m_accepted_out) {
    for (;;) {
        const int fd = accept(data->listen_fd, NULL, NULL);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            LOGGER_PERROR("control", "accept");
            return;
        }

        ControlConn* conn = conn_slot_at(data, -1);
        if (conn == NULL || (size_t)fd >= fd_capacity) {
            // busy policy: close immediately, the client sees EOF
            close(fd);
            continue;
        }
        if (set_nonblocking(fd) == -1) {
            LOGGER_PERROR("control", "set_nonblocking");
            close(fd);
            continue;
        }
        if (conn_slot_init(conn, fd) == -1) {
            close(fd);
            continue;
        }
        const int err = Vec_Fd_push_back(m_accepted_out, &conn->fd);
        assert(err != -1);
        (void)err;
    }
}

void Control_read(ControlData* data, const Vec_Fd* m_conns_reading, SparseSet_bool* m_close_fds) {
    for (size_t i = 0; i < Vec_Fd_size(m_conns_reading); i++) {
        const Fd fd = *Vec_Fd_at(m_conns_reading, i);
        if (SparseSet_bool_contains(m_close_fds, (size_t)fd)) {
            continue;
        }
        ControlConn* conn = conn_slot_at(data, fd);
        if (conn == NULL) {
            assert(false && "polled control fd must have a slot");
            continue;
        }
        if (reader_recv(&conn->reader, fd, &conn->read_q) == -1) {
            *SparseSet_bool_activate(m_close_fds, (size_t)fd) = true;
        }
    }
}

static int build_status_body(const ControlData* data, const ControlStatusSnapshot* snapshot,
                             char* buf, size_t buf_size, size_t* out_len) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long long uptime_s = (long long)now.tv_sec - (long long)data->start_time.tv_sec;

    const int written = snprintf(buf, buf_size,
        "{\"pid\":%ld,\"uptime_s\":%lld,\"players_connected\":%zu,"
        "\"players_authed\":%zu,\"fds_used\":%zu,\"fds_capacity\":%zu,"
        "\"listen_port\":%d}",
        (long)getpid(), uptime_s, snapshot->players_connected,
        snapshot->players_authed, snapshot->fds_used, snapshot->fds_capacity,
        snapshot->listen_port);
    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }
    *out_len = (size_t)written;
    return 0;
}

static int process_one(ControlData* data, ControlConn* conn, const ControlStatusSnapshot* snapshot) {
    ReaderFrame* frame = ReaderFrameQueue_front(&conn->read_q);

    HtttpStatus status = HTTTP_STATUS_BAD_REQUEST;
    char status_body[512];
    const char* body = NULL;
    size_t body_len = 0;
    bool shutdown_requested = false;

    HtttpMessage parsed;
    if (frame->status == READER_FRAME_OK &&
        htttp_parse(frame->content.ptr, frame->content.length, &parsed) == 0 &&
        parsed.is_request) {
        if (strcmp(parsed.request.path, "/status") == 0) {
            if (strcmp(parsed.request.method, "GET") != 0) {
                status = HTTTP_STATUS_METHOD_NOT_ALLOWED;
            }
            else if (build_status_body(data, snapshot, status_body,
                                       sizeof(status_body), &body_len) == -1) {
                return -1;
            }
            else {
                status = HTTTP_STATUS_OK;
                body = status_body;
            }
        }
        else if (strcmp(parsed.request.path, "/shutdown") == 0) {
            if (strcmp(parsed.request.method, "POST") != 0) {
                status = HTTTP_STATUS_METHOD_NOT_ALLOWED;
            }
            else {
                status = HTTTP_STATUS_OK;
                body = "{\"ok\":true}";
                body_len = strlen(body);
                shutdown_requested = true;
            }
        }
        else if (strcmp(parsed.request.path, "/reload") == 0) {
            if (strcmp(parsed.request.method, "POST") != 0) {
                status = HTTTP_STATUS_METHOD_NOT_ALLOWED;
            }
            else {
                should_reload_config = 1; // applied atop the main loop
                status = HTTTP_STATUS_OK;
                body = "{\"ok\":true}";
                body_len = strlen(body);
            }
        }
        else {
            status = HTTTP_STATUS_NOT_FOUND;
        }
    }

    HtttpMessage response;
    response.is_request = false;
    HtttpMessageOwnership ownership;
    if (htttp_make_default_response(status, body, body_len, false,
                                    &response.response, &ownership) == -1) {
        return -1;
    }

    size_t serialized_len = 0;
    unsigned char* serialized = htttp_serialize(&response, &serialized_len);
    htttp_message_free(&response, &ownership);
    if (serialized == NULL) {
        return -1;
    }
    if (serialized_len == 0 || serialized_len > FRAME_MAX) {
        free(serialized);
        return -1;
    }

    const WriterFrame out = { serialized, serialized_len };
    const int err = WriterFrameQueue_push_back(&conn->write_q, &out);
    assert(err != -1 && "at most one response is ever staged per connection");
    (void)err;

    // only honored once the response is staged, so a failed allocation above
    // reads as "busy" to the client instead of killing the daemon silently
    conn->shutdown_on_teardown = shutdown_requested;
    conn->state = CONTROL_CONN_RESPONDING;
    return 0;
}

void Control_process(ControlData* data, const ControlStatusSnapshot* snapshot, SparseSet_bool* m_close_fds) {
    for (size_t i = 0; i < CONTROL_MAX_CONNS; i++) {
        ControlConn* conn = &data->conns[i];
        if (conn->fd == -1 || conn->state != CONTROL_CONN_READING) {
            continue;
        }
        if (SparseSet_bool_contains(m_close_fds, (size_t)conn->fd)) {
            continue;
        }
        if (ReaderFrameQueue_size(&conn->read_q) == 0) {
            continue;
        }
        if (process_one(data, conn, snapshot) == -1) {
            *SparseSet_bool_activate(m_close_fds, (size_t)conn->fd) = true;
        }
    }
}

void Control_write(ControlData* data, SparseSet_bool* m_close_fds, Vec_WriterQueueStatusEntry* m_write_qs_status) {
    for (size_t i = 0; i < CONTROL_MAX_CONNS; i++) {
        ControlConn* conn = &data->conns[i];
        if (conn->fd == -1) {
            continue;
        }
        const size_t fd = (size_t)conn->fd;
        if (SparseSet_bool_contains(m_close_fds, fd)) {
            continue; // close reclaims whatever the queue still holds
        }

        const bool has_pending = WriterFrameQueue_size(&conn->write_q) > 0 ||
                                 conn->writer.state != WRITER_IDLE;
        if (!has_pending) {
            if (conn->state == CONTROL_CONN_RESPONDING) {
                // unreachable today (a drained response closes below), kept so
                // a responding slot can never leak
                *SparseSet_bool_activate(m_close_fds, fd) = true;
            }
            continue;
        }

        if (writer_send(&conn->writer, conn->fd, &conn->write_q) == -1) {
            writer_queue_drain(&conn->write_q);
            *SparseSet_bool_activate(m_close_fds, fd) = true;
            continue;
        }

        const bool drained = WriterFrameQueue_size(&conn->write_q) == 0 &&
                             conn->writer.state == WRITER_IDLE;
        if (drained) {
            assert(conn->state == CONTROL_CONN_RESPONDING);
            // graceful close once the response is flushed; no status entry —
            // Epoll_sync_interest forbids close-set fds in the status list
            *SparseSet_bool_activate(m_close_fds, fd) = true;
            continue;
        }

        const WriterQueueStatusEntry status = {
            conn->fd,
            WriterFrameQueue_size(&conn->write_q) == WriterFrameQueue_capacity(&conn->write_q)
                ? WRITER_QUEUE_FULL
                : WRITER_QUEUE_NORMAL,
        };
        const int err = Vec_WriterQueueStatusEntry_push_back(m_write_qs_status, &status);
        assert(err != -1);
        (void)err;
    }
}

void Control_close(ControlData* data, const SparseSet_bool* m_close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(m_close_fds); i++) {
        const Fd fd = (Fd)SparseSet_bool_key_at_idx(m_close_fds, i);
        ControlConn* conn = conn_slot_at(data, fd);
        if (conn == NULL) {
            continue;
        }
        if (conn->shutdown_on_teardown) {
            LOGGER_LOG(LOG_INFO, "control", "shutdown requested over the control channel");
            running = 0;
        }
        conn_slot_free(conn);
    }
}
