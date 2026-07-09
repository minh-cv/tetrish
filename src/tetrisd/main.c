#include "dtor.h"
#include "epollmanip.h"
#include "tetrissh.h"
#include "client.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

#define MAX_EVENTS 64
#define MAX_CLIENTS 1024

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

int main(int argc, char** argv) {
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

    TetrishCredential credential;
    if (tetrish_credential_init(&credential, "auth/server_private_key.pem", "auth/server_signed.crt") == -1) {
        perror("credential init");
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, tetrish_credential_free, &credential);

    int port = (argc > 1) ? atoi(argv[1]) : 4321;
    const char *address = (argc > 2) ? argv[2] : "localhost";

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return 1;
    }
    DTOR_INSERT(dtor, close_ptr, &listen_fd);

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        DTOR_RETURN(dtor, 1);
    }

    if (set_nonblocking(listen_fd) == -1) {
        perror("set_nonblocking listen_fd");
        DTOR_RETURN(dtor, 1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (strcmp(address, "localhost") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (strcmp(address, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        inet_pton(AF_INET, address, &addr.sin_addr);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        DTOR_RETURN(dtor, 1);
    }

    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        DTOR_RETURN(dtor, 1);
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, close_ptr, &epoll_fd);

    struct epoll_event listen_ev = {0};
    listen_ev.events = EPOLLIN;
    listen_ev.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_ev) == -1) {
        perror("epoll_ctl listen_fd");
        DTOR_RETURN(dtor, 1);
    }

    printf("server listening on port %d\n", port);

    struct client *clients[MAX_CLIENTS] = {0};
    while (running) {
        struct epoll_event events[MAX_EVENTS];

        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
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
                        perror("accept4");
                        break;
                    }

                    if (client_fd >= MAX_CLIENTS) {
                        errno = EMFILE;
                        perror("client_fd too large");
                        continue;
                    }

                    if (set_nonblocking(client_fd) == -1) {
                        perror("set_nonblocking");
                        close(client_fd);
                        continue;
                    }

                    if ((clients[client_fd] = add_client(epoll_fd, client_fd)) == NULL) {
                        perror("add_client");
                        close(client_fd);
                        continue;
                    }
                    printf("accepted client fd=%d\n", client_fd);
                }
                continue;
            }

            if (fd < 0 || fd >= MAX_CLIENTS || clients[fd] == NULL) {
                continue;
            }

            struct client *c = clients[fd];

            if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                printf("closing client fd=%d\n", fd);
                close_client(epoll_fd, c);
                clients[fd] = NULL;
                continue;
            }

            if ((evs & EPOLLIN) && c->state != CLIENT_WRITING) {
                if (handle_read(epoll_fd, c, &credential) == -1) {
                    printf("read failure, closing client fd=%d\n", fd);
                    close_client(epoll_fd, c);
                    clients[fd] = NULL;
                    continue;
                }
            }

            if ((evs & EPOLLOUT) && c->state == CLIENT_WRITING) {
                if (handle_write(epoll_fd, c) == -1) {
                    printf("write failure, closing client fd=%d\n", fd);
                    close_client(epoll_fd, c);
                    clients[fd] = NULL;
                    continue;
                }
            }
        }
    }

    for (int fd = 0; fd < MAX_CLIENTS; fd++) {
        if (clients[fd]) {
            close_client(epoll_fd, clients[fd]);
        }
    }

    DTOR_RETURN(dtor, 0);
}