#include "common.h"
#include "dtor.h"
#include "htttp.h"
#include "tetrissh.h"

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

enum client_state {
    CLIENT_READING_LEN,
    CLIENT_READING_BODY,
    CLIENT_WRITING,
};

enum client_auth_state {
    CLIENT_AUTH_NONCE,
    CLIENT_AUTH_SYMKEY,
    CLIENT_AUTH_SUCCESS,
};

typedef struct frame {
    uint8_t len_buf[4];
    uint32_t len_used;

    unsigned char* buf;
    uint32_t len;
    uint32_t used;

    bool is_heap_allocated;
} frame;

struct client {
    int fd;
    enum client_state state;
    enum client_auth_state auth_state;

    //! @brief a stack-like object containing frame, tracked by `frame_count`
    frame frame[5];

    //! @brief the number of frame objects currently available
    unsigned int frame_count;

    //! @brief the number of frame objects left to read/write from
    unsigned int frame_active;

    unsigned char session_key[SESSION_KEY_LEN];
};

static struct client *clients[MAX_CLIENTS];
static TetrishCredential credential;

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(tetrish_credential_free)
static DTOR_WRAPPER_DEFINE(htttp_message_free)

static void frame_free(struct frame* frame) {
    if (frame->is_heap_allocated) {
        free(frame->buf);
    }
}

static void client_pop_frame(struct client* c, unsigned int count) {
    assert(count <= c->frame_count);
    for (; count > 0; count--) {
        c->frame_count--;
        frame_free(&c->frame[c->frame_count]);
    }
}

static void client_push_frame(struct client* c, const struct frame frames[], unsigned int count) {
    assert(count <= 5 - c->frame_count);
    for (unsigned int i = 0; i < count; i++) {
        c->frame[c->frame_count] = frames[count - i - 1];
        c->frame_count++;
    }
}

static void client_transist_state(struct client* c, enum client_auth_state auth_state, enum client_state write_state, unsigned int frame_active) {
    assert(frame_active <= 5 - c->frame_count);
    c->auth_state = auth_state;
    c->state = write_state;
    c->frame_active = frame_active;

    if (write_state == CLIENT_READING_LEN) {
        memset(c->frame + c->frame_count, 0, sizeof(struct frame)*frame_active);
    }
}

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

static void close_client(int epoll_fd, struct client *c) {
    assert(c != NULL);

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);

    client_pop_frame(c, c->frame_count);

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

    if (client_fd >= MAX_CLIENTS) {
        free(c);
        errno = EMFILE;
        return -1;
    }

    c->fd = client_fd;
    client_transist_state(c, CLIENT_AUTH_NONCE, CLIENT_READING_LEN, 1);

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

static int print_client_message(SessionKey* key, int fd, struct frame* frame) {
    uint32_t out_len;
    unsigned char* buf = tetrish_session_decrypt(key, frame->buf, frame->len, &out_len);

    if (buf == NULL) {
        fprintf(stderr, "cannot print message from client %d\n", fd);
        return -1;
    }

    printf("client %d sent: %.*s\n", fd, (int)out_len, buf);
    fflush(stdout);
    free(buf);
    return 0;
}

static int transist_read(int epoll_fd, struct client* c) {
    switch (c->auth_state) {
        case CLIENT_AUTH_NONCE: {
            assert(c->frame_count == 1);

            if (mod_epoll_events(epoll_fd, c->fd, EPOLLOUT | EPOLLRDHUP) == -1) {
                return -1;
            }

            struct frame frames[2] = {0};
            struct frame* frame_nonce = &frames[0];

            frame_nonce->is_heap_allocated = true;
            struct frame* f = &c->frame[0];
            if ((frame_nonce->buf = tetrish_server_sign_nonce(f->buf, f->len, credential.private_key, &frame_nonce->len)) == NULL) {
                return -1;
            }
            client_pop_frame(c, 1);
            encode_u32_be(frame_nonce->len_buf, frame_nonce->len);

            struct frame frame_auth = {0};
            frame_auth.is_heap_allocated = false;
            frame_auth.buf = credential.certificate;
            frame_auth.len = credential.certificate_len;
            encode_u32_be(frame_auth.len_buf, frame_auth.len);

            frames[1] = frame_auth;

            client_push_frame(c, frames, sizeof(frames)/sizeof(struct frame));
            client_transist_state(c, CLIENT_AUTH_SYMKEY, CLIENT_WRITING, 2);
            return 0;
        }
        case CLIENT_AUTH_SYMKEY: {
            assert(c->frame_count == 1);

            struct frame* f = &c->frame[0];
            unsigned char* buf;
            uint32_t len;
            if ((buf = tetrish_server_decrypt_session_key(f->buf, f->len, &credential, &len)) == NULL) {
                return -1;
            }
            client_pop_frame(c, 1);
            assert(len == SESSION_KEY_LEN);
            memcpy(c->session_key, buf, SESSION_KEY_LEN);
            free(buf);

            client_transist_state(c, CLIENT_AUTH_SUCCESS, CLIENT_READING_LEN, 1);
            return 0;
        }
        case CLIENT_AUTH_SUCCESS: {
            assert(c->frame_count == 1);

            if (mod_epoll_events(epoll_fd, c->fd, EPOLLOUT | EPOLLRDHUP) == -1 || print_client_message(&c->session_key, c->fd, &c->frame[0]) == -1) {
                return -1;
            }

            // for now, assume the response message does not use the frame.
            client_pop_frame(c, 1);
            DTOR_DEFINE(dtor, 10);

            htttp_message_t message;
            if (htttp_make_response(&message, 200, "OK", "Accepted", "text") == -1) {
                DTOR_RETURN(dtor, -1);
            }
            DTOR_INSERT(dtor, htttp_message_free, &message);

            unsigned char* buffer;
            size_t length;
            if (htttp_serialize(&message, &buffer, &length) == -1) {
                perror("serialize");
                DTOR_RETURN(dtor, -1);
            }
            DTOR_INSERT(dtor, free, buffer);

            size_t out_len_sz;
            unsigned char* out_buffer = session_encrypt(c->session_key, buffer, length, &out_len_sz);

            if (out_buffer == NULL) {
                DTOR_RETURN(dtor, -1);
            }

            if (out_len_sz > FRAME_MAX) {
                fprintf(stderr, "Message too long");
                free(out_buffer);
                DTOR_RETURN(dtor, -1);
            }

            struct frame frame = {0};
            frame.is_heap_allocated = true;
            frame.buf = out_buffer;
            frame.len = (uint32_t)out_len_sz;
            encode_u32_be(frame.len_buf, frame.len);
            client_push_frame(c, &frame, 1);
            client_transist_state(c, CLIENT_AUTH_SUCCESS, CLIENT_WRITING, 1);

            return 0;
        }
    }

    return -1;
}

static int handle_read(int epoll_fd, struct client *c) {
    assert(c->frame_active != 0);
    
    for (;;) {
        if (c->state == CLIENT_READING_LEN) {
            struct frame* f = &c->frame[c->frame_count];
            f->is_heap_allocated = true;

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

            c->frame_count++;
            c->state = CLIENT_READING_BODY;
        }

        if (c->state == CLIENT_READING_BODY) {
            struct frame* f = &c->frame[c->frame_count - 1];
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

            if (transist_read(epoll_fd, c) == -1) {
                return -1;
            }
            
            return 0;
        }

        return 0;
    }
}

static int handle_write(int epoll_fd, struct client *c) {
    while (c->frame_active > 0) {
        struct frame *f = &c->frame[c->frame_count - 1];

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

    client_transist_state(c, c->auth_state, CLIENT_READING_LEN, 1);

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
        DTOR_RETURN(dtor, 1);
    }

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