#include "acceptor.h"
#include "logger.h"
#include "socket.h"
#include <stdio.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    Renders a peer address for the connection log. The buffer is static because
    the only caller consumes it immediately, inside one log call.
*/
static const char* peer_name(const struct sockaddr_storage* peer, socklen_t peer_len) {
    static char text[INET6_ADDRSTRLEN + 8];

    if (peer_len == 0) {
        return "unknown";
    }
    if (peer->ss_family == AF_INET) {
        const struct sockaddr_in* const in = (const struct sockaddr_in*)peer;
        char host[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)) == NULL) {
            return "unknown";
        }
        snprintf(text, sizeof(text), "%s:%u", host, (unsigned)ntohs(in->sin_port));
        return text;
    }
    if (peer->ss_family == AF_INET6) {
        const struct sockaddr_in6* const in6 = (const struct sockaddr_in6*)peer;
        char host[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)) == NULL) {
            return "unknown";
        }
        snprintf(text, sizeof(text), "[%s]:%u", host, (unsigned)ntohs(in6->sin6_port));
        return text;
    }
    return "unknown";
}

int Acceptor_init(Acceptor* data, const char* address, int port, size_t max_entries) {
    const int fd = prepare_socket(address, port);
    if (fd == -1) {
        return -1;
    }
    if (Vec_Fd_init(&data->accepted, max_entries) == -1) {
        close(fd);
        return -1;
    }
    data->listen_fd = fd;
    return 0;
}

void Acceptor_free(Acceptor* data) {
    Vec_Fd_free(&data->accepted);
    close(data->listen_fd);
    data->listen_fd = -1;
}

void Acceptor_reset(Acceptor* data) {
    Vec_Fd_reset(&data->accepted);
}

void Acceptor_accept(Acceptor* data, size_t m_fd_limit, Vec_Fd* m_accepted_out, bool* should_stop_accepting) {
    *should_stop_accepting = false;
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(data->listen_fd, (struct sockaddr*)&peer, &peer_len);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                *should_stop_accepting = true;
            }
            LOGGER_PERROR("acceptor", "accept");
            return;
        }

        if ((size_t)fd >= m_fd_limit) {
            LOGGER_LOG(LOG_WARN, "acceptor", "fd=%d exceeds table capacity, dropping", fd);
            close(fd);
            *should_stop_accepting = true;
            return;
        }
        if (set_nonblocking(fd) == -1) {
            LOGGER_PERROR("acceptor", "set_nonblocking");
            close(fd);
            continue;
        }

        // the spec requires every connection event to be logged; this is the
        // only place a connection begins
        LOGGER_LOG(LOG_INFO, "acceptor", "accepted fd=%d from %s", fd, peer_name(&peer, peer_len));

        const int err = Vec_Fd_push_back(m_accepted_out, &fd);
        assert(err != -1);
        (void)err;
    }
}
