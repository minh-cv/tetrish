#include "dtor.h"
#include "htttp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 64
#define MAX_CLIENTS 1024
#define MAX_MESSAGE_SIZE 4096

static volatile sig_atomic_t running = 1;

static void handle_signal(int signo) {
    (void)signo;
    running = 0;
}

enum client_state {
    CLIENT_READING_LEN,
    CLIENT_READING_BODY,
    CLIENT_WRITING
};

struct client {
    int fd;
    enum client_state state;

    uint8_t len_buf[4];
    size_t len_used;

    uint8_t in_buf[MAX_MESSAGE_SIZE];
    uint32_t in_len;
    size_t in_used;

    uint8_t out_buf[4 + MAX_MESSAGE_SIZE];
    size_t out_len;
    size_t out_sent;
};

static struct client *clients[MAX_CLIENTS];

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

static uint32_t decode_u32_be(const uint8_t buf[4]) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           ((uint32_t)buf[3] << 0);
}

static void encode_u32_be(uint8_t buf[4], uint32_t value) {
    buf[0] = (uint8_t)((value >> 24) & 0xFF);
    buf[1] = (uint8_t)((value >> 16) & 0xFF);
    buf[2] = (uint8_t)((value >> 8) & 0xFF);
    buf[3] = (uint8_t)((value >> 0) & 0xFF);
}

static void close_client(int epoll_fd, struct client *c) {
    if (!c) return;

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);

    if (c->fd >= 0 && c->fd < MAX_CLIENTS) {
        clients[c->fd] = NULL;
    }

    close(c->fd);
    free(c);
}

static int mod_epoll_events(int epoll_fd, int fd, uint32_t events) {
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static int add_client(int epoll_fd, int client_fd) {
    struct client *c = calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }

    c->fd = client_fd;
    c->state = CLIENT_READING_LEN;

    if (client_fd >= MAX_CLIENTS) {
        free(c);
        errno = EMFILE;
        return -1;
    }

    clients[client_fd] = c;

    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = client_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        clients[client_fd] = NULL;
        free(c);
        return -1;
    }

    return 0;
}

static int queue_reply(struct client *c, const unsigned char *msg, size_t msglen) {
    if (msglen > MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    encode_u32_be(c->out_buf, (uint32_t)msglen);
    memcpy(c->out_buf + 4, msg, msglen);

    c->out_len = msglen + 4;
    c->out_sent = 0;
    c->state = CLIENT_WRITING;

    return 0;
}

static int handle_read(int epoll_fd, struct client *c) {
    for (;;) {
        if (c->state == CLIENT_READING_LEN) {
            ssize_t n = recv(c->fd,
                             c->len_buf + c->len_used,
                             4 - c->len_used,
                             0);

            if (n == 0) {
                return -1;
            }
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                }
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }

            c->len_used += (size_t)n;

            if (c->len_used < 4) {
                return 0;
            }

            c->in_len = decode_u32_be(c->len_buf);
            c->len_used = 0;
            c->in_used = 0;

            if (c->in_len == 0 || c->in_len > MAX_MESSAGE_SIZE) {
                fprintf(stderr, "invalid message length from fd %d: %u\n",
                        c->fd, c->in_len);
                return -1;
            }

            c->state = CLIENT_READING_BODY;
        }

        if (c->state == CLIENT_READING_BODY) {
            ssize_t n = recv(c->fd,
                             c->in_buf + c->in_used,
                             c->in_len - c->in_used,
                             0);

            if (n == 0) {
                return -1;
            }
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                }
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }

            c->in_used += (size_t)n;

            if (c->in_used < c->in_len) {
                return 0;
            }

            printf("client %d sent: %.*s\n", c->fd, (int)c->in_len, c->in_buf);
            fflush(stdout);

            htttp_message_t message;
            htttp_make_response(&message, 200, "OK", "Accepted", "text");
            unsigned char* buffer;
            size_t length;
            htttp_serialize(&message, &buffer, &length);
            htttp_message_free(&message);

            if (queue_reply(c, buffer, length) == -1) {
                return -1;
            }

            if (mod_epoll_events(epoll_fd, c->fd,
                                 EPOLLIN | EPOLLOUT | EPOLLRDHUP) == -1) {
                return -1;
            }

            return 0;
        }

        return 0;
    }
}

static int handle_write(int epoll_fd, struct client *c) {
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->fd,
                         c->out_buf + c->out_sent,
                         c->out_len - c->out_sent,
                         0);

        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        c->out_sent += (size_t)n;
    }

    c->out_len = 0;
    c->out_sent = 0;
    c->state = CLIENT_READING_LEN;
    c->len_used = 0;
    c->in_len = 0;
    c->in_used = 0;

    if (mod_epoll_events(epoll_fd, c->fd, EPOLLIN | EPOLLRDHUP) == -1) {
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
        return 1;
    }

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

                    if (set_nonblocking(client_fd) == -1) {
                        perror("set_nonblocking");
                        close(client_fd);
                        continue;
                    }

                    if (add_client(epoll_fd, client_fd) == -1) {
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
                continue;
            }

            if ((evs & EPOLLIN) && c->state != CLIENT_WRITING) {
                if (handle_read(epoll_fd, c) == -1) {
                    printf("read failure, closing client fd=%d\n", fd);
                    close_client(epoll_fd, c);
                    continue;
                }
            }

            if ((evs & EPOLLOUT) && c->state == CLIENT_WRITING) {
                if (handle_write(epoll_fd, c) == -1) {
                    printf("write failure, closing client fd=%d\n", fd);
                    close_client(epoll_fd, c);
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