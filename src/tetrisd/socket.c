#include "socket.h"
#include "dtor.h"
#include "logger.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/un.h>

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static DTOR_WRAPPER_DEFINE(freeaddrinfo)

int prepare_socket(const char* address, int port) {
    DTOR_DEFINE(dtor, 10);
    
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (strcmp(address, "0.0.0.0") == 0) {
        address = NULL;
        hints.ai_flags = AI_PASSIVE;
    }

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);    

    struct addrinfo* res;
    int rc = getaddrinfo(address, port_str, &hints, &res);
    if (rc != 0) {
        LOGGER_LOG(LOG_ERROR, "socket", "getaddrinfo: %s", gai_strerror(rc));
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, freeaddrinfo, res);

    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        int listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

        if (listen_fd == -1) {
            LOGGER_PERROR("socket", "socket");
            continue;
        }

        int opt = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            LOGGER_PERROR("socket", "setsockopt");
            close(listen_fd); continue;
        }

        if (set_nonblocking(listen_fd) == -1) {
            LOGGER_PERROR("socket", "set_nonblocking listen_fd");
            close(listen_fd); continue;
        }

        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
            LOGGER_PERROR("socket", "bind");
            close(listen_fd); continue;
        }

        if (listen(listen_fd, SOMAXCONN) == -1) {
            LOGGER_PERROR("socket", "listen");
            close(listen_fd); continue;
        }

        DTOR_RETURN(dtor, listen_fd);

    }

    LOGGER_LOG(LOG_ERROR, "socket", "cannot make socket");
    DTOR_RETURN(dtor, -1);
}

int prepare_logger_socket(const char* log_ipc) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        LOGGER_PERROR("logger", "socket");
        return -1;
    }

    if (set_nonblocking(sockfd) == -1) {
        LOGGER_PERROR("logger", "set_nonblocking sockfd");
        close(sockfd);
        return -1;
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    size_t log_path_length = strlen(log_ipc);
    if (log_path_length >= sizeof(addr.sun_path)) {
        LOGGER_LOG(LOG_ERROR, "logger", "log_path too long");
        close(sockfd);
        return -1;
    }

    memcpy(addr.sun_path, log_ipc, log_path_length);
    addr.sun_path[log_path_length] = '\0';

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1 && errno != EINPROGRESS) {
        LOGGER_PERROR("logger", "connect");
        close(sockfd);
        return -1;
    }

    return sockfd;
}
