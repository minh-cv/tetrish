#include "connector.h"
#include "dtor.h"
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(freeaddrinfo)

static int connect_any(const char* address, int port) {
    DTOR_DEFINE(dtor, 4);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    const int written = snprintf(port_str, sizeof(port_str), "%d", port);
    if (written < 0 || (size_t)written >= sizeof(port_str)) {
        fprintf(stderr, "port out of range: %d\n", port);
        DTOR_RETURN(dtor, -1);
    }

    struct addrinfo* res;
    const int rc = getaddrinfo(address, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, freeaddrinfo, res);

    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        const int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            DTOR_RETURN(dtor, fd);
        }
        close(fd);
    }

    fprintf(stderr, "cannot connect to %s:%d\n", address, port);
    DTOR_RETURN(dtor, -1);
}

static int set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }
    return 0;
}

int Connector_init(Connector* data, const char* address, int port, const char* ca_path) {
    data->server_fd = -1;

    const int fd = connect_any(address, port);
    if (fd == -1) {
        return -1;
    }

    if (tetrish_client_handshake(fd, ca_path, &data->key) == -1) {
        fprintf(stderr, "handshake failed\n");
        close(fd);
        return -1;
    }

    // only now, so the blocking handshake above sees a blocking socket
    if (set_nonblocking(fd) == -1) {
        close(fd);
        return -1;
    }

    data->server_fd = fd;
    return 0;
}

void Connector_free(Connector* data) {
    if (data->server_fd != -1) {
        close(data->server_fd);
        data->server_fd = -1;
    }
    memset(data->key, 0, sizeof(data->key));
}
