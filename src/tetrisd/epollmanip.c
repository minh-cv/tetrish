#include "epollmanip.h"
#include "client.h"
#include "state.h"
#include "tetrissh.h"
#include <assert.h>
#include <sched.h>
#include <sys/epoll.h>

static int mod_epoll_events(int epoll_fd, int fd, uint32_t events) {
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

void close_client(int epoll_fd, struct client *c) {
    assert(c != NULL);

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);

    client_pop_frame(c, c->frame_count);

    close(c->fd);
    free(c);
}

struct client* add_client(int epoll_fd, int client_fd) {
    struct client *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }

    c->fd = client_fd;
    client_transit_state(c, CLIENT_AUTH_NONCE, CLIENT_READING_LEN, 1);

    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = client_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        free(c);
        return NULL;
    }

    return c;
}

int handle_read(int epoll_fd, struct client *c, TetrishCredential* credential) {
    assert(c->frame_active != 0);
    
    for (;;) {
        if (c->state == CLIENT_READING_LEN) {
            struct frame actual_frame = {0};
            struct frame *f = &actual_frame;

            ssize_t n = recv(c->fd,
                             f->len_buf + f->len_used,
                             sizeof(f->len_buf) - f->len_used,
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

            f->len_used += (uint32_t)n;

            if (f->len_used < sizeof(f->len_buf)) {
                return 0;
            }

            f->len = decode_u32_be(f->len_buf);

            if (f->len == 0 || f->len > FRAME_MAX) {
                fprintf(stderr, "invalid message length from fd %d: %u\n",
                        c->fd, f->len);
                return -1;
            }
            
            f->buf = malloc(f->len);
            if (f->buf == NULL) {
                return -1;
            }

            f->is_heap_allocated = true;
            client_push_frame(c, f, 1);
            c->state = CLIENT_READING_BODY;
        }

        if (c->state == CLIENT_READING_BODY) {
            struct frame* f = client_get_top_frame(c);
            ssize_t n = recv(c->fd,
                             f->buf + f->used,
                             f->len - f->used,
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

            f->used += (uint32_t)n;

            if (f->used < f->len) {
                return 0;
            }

            c->frame_active--;
            if (0 != c->frame_active) {
                c->state = CLIENT_READING_LEN;
                return 0;
            }

            if (transit_read(c, credential) == -1) {
                return -1;
            }

            if (c->state == CLIENT_WRITING) {
                if (mod_epoll_events(epoll_fd, c->fd, EPOLLOUT | EPOLLRDHUP) == -1) {
                    return -1;
                }
            }
            
            return 0;
        }

        return 0;
    }
}

int handle_write(int epoll_fd, struct client *c) {
    while (c->frame_active > 0) {
        struct frame *f = client_get_top_frame(c);

        while (f->len_used < sizeof(f->len_buf)) {
            ssize_t n = send(c->fd,
                             f->len_buf + f->len_used,
                             sizeof(f->len_buf) - f->len_used,
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

            f->len_used += (uint32_t)n;
        }

        while (f->used < f->len) {
            ssize_t n = send(c->fd,
                             f->buf + f->used,
                             f->len - f->used,
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

            f->used += (uint32_t)n;
        }
        client_pop_frame(c, 1);
        c->frame_active--;
    }

    client_transit_state(c, c->auth_state, CLIENT_READING_LEN, 1);

    if (mod_epoll_events(epoll_fd, c->fd, EPOLLIN | EPOLLRDHUP) == -1) {
        return -1;
    }

    return 0;
}