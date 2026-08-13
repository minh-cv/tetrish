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

// one request in flight per tick; the connection stays open for the next
#define CONTROL_QUEUE_CAP 1u

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

static int conn_init(ControlConn* conn, Fd fd) {
    if (ReaderFrameQueue_init(&conn->read_q, CONTROL_QUEUE_CAP) == -1) {
        return -1;
    }
    if (WriterFrameQueue_init(&conn->write_q, CONTROL_QUEUE_CAP) == -1) {
        ReaderFrameQueue_free(&conn->read_q);
        return -1;
    }
    reader_init(&conn->reader);
    writer_init(&conn->writer);
    conn->shutdown_requested = false;
    conn->fd = fd;
    return 0;
}

static void conn_free(ControlConn* conn) {
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

    data->conn.fd = -1;
    data->listen_fd = listen_fd;
    data->ipc_path = control_ipc;
    DTOR_RETURN(dtor, 0);
}

void Control_free(ControlData* data) {
    if (data->conn.fd != -1) {
        conn_free(&data->conn);
    }
    close(data->listen_fd);
    data->listen_fd = -1;
    unlink_path((char*)data->ipc_path);
}

void Control_reset(ControlData* data) {
    if (data->conn.fd != -1) {
        reader_queue_drain(&data->conn.read_q);
    }
}

Fd Control_accept(ControlData* data, size_t fd_capacity) {
    Fd accepted = -1;
    for (;;) {
        const int fd = accept(data->listen_fd, NULL, NULL);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return accepted;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            LOGGER_PERROR("control", "accept");
            return accepted;
        }

        if (data->conn.fd != -1 || accepted != -1) {
            close(fd);
            continue;
        }
        if ((size_t)fd >= fd_capacity) {
            LOGGER_LOG(LOG_WARN, "control", "fd=%d is out of the epoll table, dropping", fd);
            close(fd);
            continue;
        }
        if (set_nonblocking(fd) == -1) {
            LOGGER_PERROR("control", "set_nonblocking");
            close(fd);
            continue;
        }
        if (conn_init(&data->conn, fd) == -1) {
            close(fd);
            continue;
        }
        accepted = fd;
    }
}

void Control_read(ControlData* data, SparseSet_bool* m_close_fds) {
    ControlConn* conn = &data->conn;
    if (conn->fd == -1 || SparseSet_bool_contains(m_close_fds, (size_t)conn->fd)) {
        return;
    }
    if (reader_recv(&conn->reader, conn->fd, &conn->read_q) == -1) {
        *SparseSet_bool_activate(m_close_fds, (size_t)conn->fd) = true;
    }
}

static int build_status_body(const ControlStatusSnapshot* snapshot,
                             char* buf, size_t buf_size, size_t* out_len) {
    const int written = snprintf(buf, buf_size,
        "{\"pid\":%ld,\"players_connected\":%zu,"
        "\"players_authed\":%zu,\"players_capacity\":%zu,"
        "\"fds_used\":%zu,\"fds_capacity\":%zu,\"listen_port\":%d}",
        (long)getpid(), snapshot->players_connected,
        snapshot->players_authed, snapshot->players_capacity,
        snapshot->fds_used, snapshot->fds_capacity, snapshot->listen_port);
    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }
    *out_len = (size_t)written;
    return 0;
}

static int process_one(ControlConn* conn, const ReaderFrame* frame,
                       const ControlStatusSnapshot* snapshot) {
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
            else if (build_status_body(snapshot, status_body,
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
    if (WriterFrameQueue_push_back(&conn->write_q, &out) == -1) {
        free(serialized);
        return -1;
    }

    // only honored once the response is staged, so a failed allocation above
    // reads as "busy" to the client instead of killing the daemon silently
    if (shutdown_requested) {
        conn->shutdown_requested = true;
    }
    return 0;
}

void Control_process(ControlData* data, const ControlStatusSnapshot* snapshot, SparseSet_bool* m_close_fds) {
    ControlConn* conn = &data->conn;
    if (conn->fd == -1 || SparseSet_bool_contains(m_close_fds, (size_t)conn->fd)) {
        return;
    }

    const size_t count = ReaderFrameQueue_size(&conn->read_q);
    for (size_t i = 0; i < count; i++) {
        const ReaderFrame* frame = ReaderFrameQueue_at(&conn->read_q, i);
        if (process_one(conn, frame, snapshot) == -1) {
            *SparseSet_bool_activate(m_close_fds, (size_t)conn->fd) = true;
            return;
        }
    }
}

bool Control_wants_write(const ControlData* data) {
    return data->conn.fd != -1 &&
           (WriterFrameQueue_size(&data->conn.write_q) > 0 ||
            data->conn.writer.state != WRITER_IDLE);
}

void Control_write(ControlData* data, SparseSet_bool* m_close_fds) {
    ControlConn* conn = &data->conn;
    if (conn->fd == -1) {
        return;
    }
    const size_t fd = (size_t)conn->fd;
    if (SparseSet_bool_contains(m_close_fds, fd)) {
        return; // close reclaims whatever the queue still holds
    }
    if (!Control_wants_write(data)) {
        return;
    }

    if (writer_send(&conn->writer, conn->fd, &conn->write_q) == -1) {
        writer_queue_drain(&conn->write_q);
        *SparseSet_bool_activate(m_close_fds, fd) = true;
        return;
    }

    if (conn->shutdown_requested && !Control_wants_write(data)) {
        LOGGER_LOG(LOG_INFO, "control", "shutdown requested over the control channel");
        running = 0;
    }
}

void Control_close(ControlData* data, const SparseSet_bool* m_close_fds) {
    ControlConn* conn = &data->conn;
    if (conn->fd == -1 || !SparseSet_bool_contains(m_close_fds, (size_t)conn->fd)) {
        return;
    }
    conn_free(conn);
}
