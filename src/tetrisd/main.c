#include "config_var.h"
#ifndef TETRISH_TETRISD_NO_DAEMON
#include "daemon.h"
#endif
#include "dtor.h"
#include "epollmanip.h"
#include "logger.h"
#include "tetrissh.h"

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

int main() {
    DTOR_DEFINE(dtor, 20);

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
    
    TetrishCredential credential;
    if (tetrish_credential_init(&credential, cfg.key_path, cfg.cert_path) == -1) {
        perror("credential init");
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
                        close(client_fd);
                        continue;
                    }

                    if (handle_client_event(epoll_fd, &clients[client_fd]) != -1) {
                        LOGGER_LOG(LOG_INFO, "accept", "accepted client fd=%d", client_fd);
                    }
                }
                continue;
            }

            if (fd >= cfg.max_clients) {
                // TODO: logging
                continue;
            }
            if (fd < 0 || clients[fd].tag == CLIENT_TAG_INACTIVE) {
                assert(false);
                continue;
            }

            if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                LOGGER_LOG(LOG_INFO, "client", "closing client fd=%d", fd);
                close_client(epoll_fd, &clients[fd]);
                continue;
            }

            if (evs & (EPOLLIN | EPOLLOUT)) {
                handle_client_event(epoll_fd, &clients[fd]);
                continue;
            }
        }
    }

    for (int fd = 0; fd < cfg.max_clients; fd++) {
        if (clients[fd].tag != CLIENT_TAG_INACTIVE) {
            close_client(epoll_fd, &clients[fd]);
        }
    }

    DTOR_RETURN(dtor, 0);
}