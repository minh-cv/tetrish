// struct ucred and SOCK_NONBLOCK are Linux extensions; the control plane is
// the one place in the daemon that needs peer credentials
#define _GNU_SOURCE

#include "ctl.h"
#include "htttp.h"
#include "logger.h"
#include "tetrissh.h"
#include "wire.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_BODY_MAX 1024

static const char* lifecycle_name(ServerLifecycle lifecycle) {
    switch (lifecycle) {
    case SERVER_RUNNING: return "running";
    case SERVER_DRAINING: return "draining";
    case SERVER_STOPPING: return "stopping";
    }
    return "unknown";
}

/*
    A live socket file means another daemon owns this path; only a refused
    connect proves it is a leftover. Unlinking without the probe would let a
    second daemon silently take over a running daemon's control channel.
*/
static int clear_stale_socket(const char* path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path));

    const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe == -1) {
        LOGGER_PERROR("ctl", "socket");
        return -1;
    }

    const int rc = connect(probe, (struct sockaddr*)&addr, sizeof(addr));
    close(probe);

    if (rc == 0) {
        LOGGER_LOG(LOG_ERROR, "ctl", "another tetrisd owns %s", path);
        return -1;
    }
    if (errno != ECONNREFUSED && errno != ENOENT) {
        LOGGER_PERROR("ctl", "connect probe");
        return -1;
    }
    if (unlink(path) == -1 && errno != ENOENT) {
        LOGGER_PERROR("ctl", "unlink");
        return -1;
    }
    return 0;
}

int CtlData_init(CtlData* data, const char* path, unsigned timeout_ms) {
    data->listen_fd = -1;
    data->path = NULL;
    data->timeout_ms = timeout_ms;
    data->shutdown_requested = false;
    data->drain_requested = false;

    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOGGER_LOG(LOG_ERROR, "ctl", "ctl_ipc path too long");
        return -1;
    }
    if (clear_stale_socket(path) == -1) {
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        LOGGER_PERROR("ctl", "socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path));

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        LOGGER_PERROR("ctl", "bind");
        close(fd);
        return -1;
    }
    // binding under a restrictive umask is not enough on every platform, so
    // the explicit mode is the contract the uid check backs up
    if (chmod(path, S_IRUSR | S_IWUSR) == -1) {
        LOGGER_PERROR("ctl", "chmod");
        close(fd);
        unlink(path);
        return -1;
    }
    if (listen(fd, 8) == -1) {
        LOGGER_PERROR("ctl", "listen");
        close(fd);
        unlink(path);
        return -1;
    }

    data->path = strdup(path);
    if (data->path == NULL) {
        close(fd);
        unlink(path);
        return -1;
    }
    data->listen_fd = fd;
    return 0;
}

void CtlData_free(CtlData* data) {
    if (data->listen_fd != -1) {
        close(data->listen_fd);
        data->listen_fd = -1;
    }
    if (data->path != NULL) {
        unlink(data->path);
        free(data->path);
        data->path = NULL;
    }
}

/*
    Defence in depth behind the 0600 mode: a peer that is neither the daemon's
    own user nor root is refused before its request is even read.
*/
static bool peer_allowed(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
        LOGGER_PERROR("ctl", "SO_PEERCRED");
        return false;
    }
    return cred.uid == geteuid() || cred.uid == 0;
}

static void set_timeouts(int fd, unsigned timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int send_response(int fd, HtttpStatus status, const char* body) {
    char* const owned = body == NULL ? NULL : strdup(body);
    if (body != NULL && owned == NULL) {
        return -1;
    }

    HtttpMessage message;
    memset(&message, 0, sizeof(message));
    message.is_request = false;
    HtttpMessageOwnership ownership;
    if (htttp_make_default_response(status, owned, owned == NULL ? 0 : strlen(owned), true,
                                    &message.response, &ownership) == -1) {
        return -1;
    }

    size_t length;
    unsigned char* const buffer = htttp_serialize(&message, &length);
    htttp_message_free(&message, &ownership);
    if (buffer == NULL || length == 0 || length > FRAME_MAX) {
        free(buffer);
        return -1;
    }

    const int rc = tetrish_send_frame(fd, buffer, (uint32_t)length, NULL);
    free(buffer);
    return rc;
}

static void format_status(const ServerStatus* status, char* out, size_t out_size) {
    snprintf(out, out_size,
             "pid=%d\nlifecycle=%s\naddress=%s\nport=%d\nuptime_seconds=%ld\n"
             "players_connected=%zu\nplayers_authenticated=%zu\nrooms=%zu\n"
             "max_fds=%zu\ntick_hz=%zu\naccepting=%s\n",
             (int)status->pid, lifecycle_name(status->lifecycle), status->address, status->port,
             status->uptime_seconds, status->players_connected, status->players_authenticated,
             status->rooms, status->max_fds, status->tick_hz, status->accepting ? "yes" : "no");
}

/*
    One request per connection, so routing is a flat match on method and path
    with no state to carry between calls.
*/
static int route(CtlData* data, const HtttpMessage* message, const ServerStatus* status,
                 HtttpStatus* out_status, char* body, size_t body_size) {
    if (!message->is_request) {
        *out_status = HTTTP_STATUS_BAD_REQUEST;
        snprintf(body, body_size, "expected a request\n");
        return 0;
    }

    const char* const method = message->request.method;
    const char* const path = message->request.path;

    if (strcmp(path, "/ctl/status") == 0) {
        if (strcmp(method, "GET") != 0) {
            *out_status = HTTTP_STATUS_METHOD_NOT_ALLOWED;
            snprintf(body, body_size, "use GET\n");
            return 0;
        }
        *out_status = HTTTP_STATUS_OK;
        format_status(status, body, body_size);
        return 0;
    }

    if (strcmp(path, "/ctl/shutdown") == 0) {
        if (strcmp(method, "POST") != 0) {
            *out_status = HTTTP_STATUS_METHOD_NOT_ALLOWED;
            snprintf(body, body_size, "use POST\n");
            return 0;
        }
        LOGGER_LOG(LOG_WARN, "ctl", "shutdown requested");
        data->shutdown_requested = true;
        *out_status = HTTTP_STATUS_OK;
        snprintf(body, body_size, "lifecycle=stopping\n");
        return 0;
    }

    if (strcmp(path, "/ctl/drain") == 0) {
        if (strcmp(method, "POST") != 0) {
            *out_status = HTTTP_STATUS_METHOD_NOT_ALLOWED;
            snprintf(body, body_size, "use POST\n");
            return 0;
        }
        LOGGER_LOG(LOG_WARN, "ctl", "drain requested");
        data->drain_requested = true;
        *out_status = HTTTP_STATUS_OK;
        snprintf(body, body_size, "lifecycle=draining\n");
        return 0;
    }

    *out_status = HTTTP_STATUS_NOT_FOUND;
    snprintf(body, body_size, "no such control path\n");
    return 0;
}

static void serve_one(CtlData* data, int fd, const ServerStatus* status) {
    uint32_t length;
    unsigned char* const frame = tetrish_recv_frame(fd, &length, NULL);
    if (frame == NULL) {
        LOGGER_LOG(LOG_WARN, "ctl", "control peer sent nothing usable");
        return;
    }

    HtttpMessage message;
    char body[CTL_BODY_MAX];
    HtttpStatus response_status;

    if (htttp_parse(frame, length, &message) == -1) {
        response_status = HTTTP_STATUS_BAD_REQUEST;
        snprintf(body, sizeof(body), "unparsable request\n");
    }
    else {
        LOGGER_LOG(LOG_INFO, "ctl", "admin command %s %s",
                   message.is_request ? message.request.method : "(response)",
                   message.is_request ? message.request.path : "");
        route(data, &message, status, &response_status, body, sizeof(body));
    }

    if (send_response(fd, response_status, body) == -1) {
        LOGGER_LOG(LOG_WARN, "ctl", "cannot answer control peer");
    }
    free(frame);
}

void CtlData_serve(CtlData* data, const ServerStatus* status, size_t max_connections) {
    if (data->listen_fd == -1) {
        return;
    }

    for (size_t i = 0; i < max_connections; i++) {
        const int fd = accept(data->listen_fd, NULL, NULL);
        if (fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                LOGGER_PERROR("ctl", "accept");
            }
            return;
        }

        if (!peer_allowed(fd)) {
            LOGGER_LOG(LOG_WARN, "ctl", "rejected a control peer with the wrong uid");
            send_response(fd, HTTTP_STATUS_FORBIDDEN, "not permitted\n");
            close(fd);
            continue;
        }

        // the frame calls below are blocking; the timeout is what bounds how
        // long one control peer can hold the tick
        set_timeouts(fd, data->timeout_ms);
        serve_one(data, fd, status);
        close(fd);
    }
}
