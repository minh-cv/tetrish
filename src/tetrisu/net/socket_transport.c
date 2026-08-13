#include "net/socket_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    const int fd_flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1 || fd_flags == -1 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
        fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == -1) {
        return -1;
    }
    return 0;
}

void socket_transport_init(SocketTransport* transport) {
    transport->fd = -1;
}

void socket_transport_reset(SocketTransport* transport) {
    if (transport->fd >= 0) {
        close(transport->fd);
    }
    transport->fd = -1;
}

SocketConnectResult socket_transport_connect_start(
    SocketTransport* transport,
    const char* address,
    int port,
    ClientError* error
) {
    char service[6];
    if (snprintf(service, sizeof(service), "%d", port) <= 0) {
        *error = client_error(CLIENT_ERROR_CONNECT, EINVAL, "invalid port");
        return SOCKET_CONNECT_FAILED;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* addresses = NULL;
    const int gai_error = getaddrinfo(address, service, &hints, &addresses);
    if (gai_error != 0) {
        *error = client_error(CLIENT_ERROR_DNS, gai_error, "host resolution failed");
        return SOCKET_CONNECT_FAILED;
    }

    int last_error = ECONNREFUSED;
    SocketConnectResult result = SOCKET_CONNECT_FAILED;
    for (struct addrinfo* current = addresses; current != NULL; current = current->ai_next) {
        const int fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd == -1) {
            last_error = errno;
            continue;
        }
        if (make_nonblocking(fd) == -1) {
            last_error = errno;
            close(fd);
            continue;
        }

        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            transport->fd = fd;
            result = SOCKET_CONNECT_CONNECTED;
            break;
        }
        if (errno == EINPROGRESS) {
            transport->fd = fd;
            result = SOCKET_CONNECT_IN_PROGRESS;
            break;
        }
        last_error = errno;
        close(fd);
    }
    freeaddrinfo(addresses);

    if (result == SOCKET_CONNECT_FAILED) {
        *error = client_error(CLIENT_ERROR_CONNECT, last_error, "connect failed");
    }
    return result;
}

int socket_transport_connect_finish(SocketTransport* transport, ClientError* error) {
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (getsockopt(transport->fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == -1) {
        *error = client_error(CLIENT_ERROR_CONNECT, errno, "cannot inspect connection");
        return -1;
    }
    if (socket_error != 0) {
        *error = client_error(CLIENT_ERROR_CONNECT, socket_error, "connect failed");
        return -1;
    }
    return 1;
}

ssize_t socket_transport_read(SocketTransport* transport, void* buffer, size_t capacity) {
    return recv(transport->fd, buffer, capacity, 0);
}

ssize_t socket_transport_write(SocketTransport* transport, const void* buffer, size_t length) {
#ifdef MSG_NOSIGNAL
    return send(transport->fd, buffer, length, MSG_NOSIGNAL);
#else
    return send(transport->fd, buffer, length, 0);
#endif
}
