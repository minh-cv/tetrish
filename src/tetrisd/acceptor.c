#include "acceptor.h"
#include "logger.h"
#include "socket.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

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

//! @brief render @p peer as "host:port", or "?:0" if it will not format
static void format_peer(const struct sockaddr_storage* peer, char* out, size_t out_size) {
    char host[INET6_ADDRSTRLEN];
    unsigned port = 0;

    if (peer->ss_family == AF_INET) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)peer;
        port = ntohs(in->sin_port);
        if (inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)) == NULL) {
            host[0] = '\0';
        }
    }
    else if (peer->ss_family == AF_INET6) {
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)peer;
        port = ntohs(in6->sin6_port);
        if (inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)) == NULL) {
            host[0] = '\0';
        }
    }
    else {
        host[0] = '\0';
    }

    if (snprintf(out, out_size, "%s:%u", host[0] == '\0' ? "?" : host, port) < 0) {
        out[0] = '\0';
    }
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

        char peer_name[INET6_ADDRSTRLEN + 8];
        format_peer(&peer, peer_name, sizeof(peer_name));

        if ((size_t)fd >= m_fd_limit) {
            LOGGER_LOG(LOG_WARN, "acceptor", "connection from %s rejected: fd=%d exceeds table capacity",
                       peer_name, fd);
            close(fd);
            *should_stop_accepting = true;
            return;
        }
        if (set_nonblocking(fd) == -1) {
            LOGGER_PERROR("acceptor", "set_nonblocking");
            close(fd);
            continue;
        }

        int opt = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1) {
            LOGGER_PERROR("acceptor", "setsockopt TCP_NODELAY");
            close(fd);
            continue;
        }


        const int err = Vec_Fd_push_back(m_accepted_out, &fd);
        assert(err != -1);
        (void)err;

        LOGGER_LOG(LOG_INFO, "acceptor", "connection accepted fd=%d from %s", fd, peer_name);
    }
}
