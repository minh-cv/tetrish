#include "client_auth.h"
#include "client_io.h"
#include "config_var.h"
#include "log_buf.h"
#ifndef TETRISH_TETRISD_NO_DAEMON
#include "daemon.h"
#endif
#include "dtor.h"
#include "epollmanip.h"
#include "logger.h"
#include "tetrissh.h"

#include <sys/un.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int signo) {
    (void)signo;
    running = 0;
}

static DTOR_WRAPPER_DEFINE(tetrish_credential_free)

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static int close_ptr(const int* fd) {
    return close(*fd);
}

static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(freeaddrinfo)
static DTOR_WRAPPER_DEFINE(log_buf_free)

static int prepare_socket(const char* address, int port) {
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

static int prepare_logger_socket(const char* log_ipc) {
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

static const int LOGGER_RECONNECT_BACKOFF_SEC = 5;

static int reconnect_client_logger(int epoll_fd, const char* log_ipc, int max_clients, Client* clients, LogBuf* log_buf) {
    int client_logger_fd = prepare_logger_socket(log_ipc);
    if (client_logger_fd == -1 || client_logger_fd >= max_clients) {
        close(client_logger_fd);
        return -1;
    }
    if (add_logger_client(&clients[client_logger_fd], epoll_fd, client_logger_fd, log_buf) == -1) {
        close(client_logger_fd);
        return -1;
    }
    return client_logger_fd;
}

typedef struct LoggerCtx {
    LogBuf log_buf;
    bool has_log;
} LoggerCtx;

static LoggerCtx* g_logger_ctx = NULL;

static int logger_handler_log_buf(char* string) {
    assert(g_logger_ctx != NULL);
    int retval = log_buf_append_log(&g_logger_ctx->log_buf, string);
    if (retval == -1) {
        return -1;
    }
    g_logger_ctx->has_log = true;
    return 0;
}

static void main_yield_handler(int epoll_fd, Client* c) {
    switch (c->tag) {
    case CLIENT_TAG_UNAUTHED: {
        ClientUnauthed old = c->client_unauthed;
        tetris_client_init(&c->tetris_client, old.base.fd, &old.key);
        c->tag = CLIENT_TAG_TETRIS;
        ClientIoResult result = resume_client_event(epoll_fd, c);
        if (result == CLIENT_IO_YIELD) { 
            main_yield_handler(epoll_fd, c);
        }
        return;
    }
    default:
        return;
    }
}

int main() {
    DTOR_DEFINE(dtor, 20);

    logger_set_log_handler(logger_log_null);

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        DTOR_RETURN(dtor, 1);
    }
    
    struct config_var cfg;
    if (config_var_init(&cfg) == -1) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, config_var_free, &cfg);

    LoggerCtx logger_ctx = {0};
    logger_ctx.has_log = false;
    log_buf_init(&logger_ctx.log_buf);
    g_logger_ctx = &logger_ctx;
    DTOR_INSERT(dtor, log_buf_free, &logger_ctx.log_buf);
    logger_set_log_handler(logger_handler_log_buf);

    TetrishCredential credential;
    if (tetrish_credential_init(&credential, cfg.key_path, cfg.cert_path) == -1) {
        LOGGER_PERROR("auth", "credential init");
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, tetrish_credential_free, &credential);
    
    #ifndef TETRISH_TETRISD_NO_DAEMON
        switch (incantation()) {
        case 0:
            DTOR_RETURN(dtor, 0);
        case -1:
            perror("incantation");
            DTOR_RETURN(dtor, 1);
        case 1:
            break;
        default:
            assert(false);
        }
    #endif

    int listen_fd = prepare_socket(cfg.address, cfg.port);
    if (listen_fd == -1) {
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, close_ptr, &listen_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        LOGGER_PERROR("epoll", "epoll_create1");
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, close_ptr, &epoll_fd);

    struct epoll_event listen_ev = {0};
    listen_ev.events = EPOLLIN;
    listen_ev.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_ev) == -1) {
        LOGGER_PERROR("epoll", "epoll_ctl listen_fd");
        DTOR_RETURN(dtor, 1);
    }

    LOGGER_LOG(LOG_INFO, "main", "server listening on port %d", cfg.port);

    struct Client *clients = calloc((size_t)cfg.max_clients, sizeof(*clients));
    if (clients == NULL) {
        LOGGER_LOG(LOG_ERROR, "main", "clients NULL");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, clients);

    time_t logger_last_attempt = time(NULL);
    int client_logger_fd = reconnect_client_logger(epoll_fd, cfg.log_ipc, cfg.max_clients, clients, &logger_ctx.log_buf);

    struct epoll_event* events = calloc((size_t)cfg.max_events, sizeof(*events));
    if (events == NULL) {
        LOGGER_LOG(LOG_ERROR, "main", "events NULL");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, events);
    
    while (running) {
        int n = epoll_wait(epoll_fd, events, cfg.max_events, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            LOGGER_PERROR("main", "epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd == listen_fd) {
                for (;;) {
                    int client_fd = accept(listen_fd, NULL, NULL);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        LOGGER_PERROR("accept", "accept");
                        break;
                    }

                    if (client_fd >= cfg.max_clients) {
                        errno = EMFILE;
                        LOGGER_PERROR("accept", "client_fd too large");
                        close(client_fd);
                        continue;
                    }

                    if (set_nonblocking(client_fd) == -1) {
                        LOGGER_PERROR("accept", "set_nonblocking");
                        close(client_fd);
                        continue;
                    }

                    if ((add_client(&clients[client_fd], epoll_fd, client_fd, &credential)) == -1) {
                        LOGGER_PERROR("accept", "add_client");
                        continue;
                    }
                }
                continue;
            }

            Client* c = &clients[fd];

            if (fd >= cfg.max_clients || fd < 0 || c->tag == CLIENT_TAG_INACTIVE) {
                assert(false);
                continue;
            }

            if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                LOGGER_LOG(LOG_INFO, "client", "closing client fd=%d", fd);
                close_client(epoll_fd, c);
                continue;
            }

            if (evs & (EPOLLIN | EPOLLOUT)) {
                ClientIoResult result = resume_client_event(epoll_fd, c);
                if (result != CLIENT_IO_YIELD) {
                    continue;
                }
                main_yield_handler(epoll_fd, c);
            }
        }

        if (client_logger_fd != -1 && clients[client_logger_fd].tag != CLIENT_TAG_LOGGER) {
            client_logger_fd = -1;
        }
        
        if (client_logger_fd == -1) {
            time_t last_time = logger_last_attempt;
            logger_last_attempt = time(NULL);
            if (difftime(logger_last_attempt, last_time) > LOGGER_RECONNECT_BACKOFF_SEC) {
                client_logger_fd = reconnect_client_logger(epoll_fd, cfg.log_ipc, cfg.max_clients, clients, &logger_ctx.log_buf);
            }
        }
        else if (logger_ctx.has_log) {
            ClientIoResult result = resume_client_event(epoll_fd, &clients[client_logger_fd]);
            if (result == CLIENT_IO_ERR) {
                LOGGER_LOG(LOG_INFO, "logger", "closing logger fd=%d", client_logger_fd);
                client_logger_fd = -1;
            }
            if (logger_ctx.log_buf.buffer_size == 0) {
                logger_ctx.has_log = false;
            }
        }
    }

    if (client_logger_fd != -1 && logger_ctx.has_log) {
        ClientIoResult result = resume_client_event(epoll_fd, &clients[client_logger_fd]);
        if (result == CLIENT_IO_ERR) {
            LOGGER_LOG(LOG_INFO, "logger", "closing logger fd=%d", client_logger_fd);
            client_logger_fd = -1;
        }
        if (logger_ctx.log_buf.buffer_size == 0) {
            logger_ctx.has_log = false;
        }
    }

    logger_set_log_handler(logger_log_null);
    g_logger_ctx = NULL;

    for (int fd = 0; fd < cfg.max_clients; fd++) {
        if (clients[fd].tag != CLIENT_TAG_INACTIVE) {
            close_client(epoll_fd, &clients[fd]);
        }
    }

    DTOR_RETURN(dtor, 0);
}