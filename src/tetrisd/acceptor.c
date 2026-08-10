#include "acceptor.h"
#include "logger.h"
#include "socket.h"
#include <assert.h>
#include <errno.h>
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

void Acceptor_accept(Acceptor* data, size_t m_fd_limit, Vec_Fd* m_accepted_out, bool* should_stop_accepting) {
    *should_stop_accepting = false;
    for (;;) {
        const int fd = accept(data->listen_fd, NULL, NULL);
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

        const int err = Vec_Fd_push_back(m_accepted_out, &fd);
        assert(err != -1);
        (void)err;
    }
}
