#include "common.h"
#include "dtor.h"
#include "htttp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/types.h>
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

enum client_auth_state {
    CLIENT_AUTH_NONCE,
    CLIENT_AUTH_SYMKEY,
    CLIENT_AUTH_SUCCESS,
};

struct client {
    int fd;
    enum client_state state;
    enum client_auth_state auth_state;

    uint8_t len_buf[4];
    size_t len_used;

    uint8_t in_buf[MAX_MESSAGE_SIZE];
    uint32_t in_len;
    size_t in_used;

    uint8_t out_buf[4 + MAX_MESSAGE_SIZE];
    size_t out_len;
    size_t out_sent;

    unsigned char session_key[SESSION_KEY_LEN];
};

static struct client *clients[MAX_CLIENTS];
static EVP_PKEY* private_key;
static unsigned char* certificate;
static size_t certificate_len;

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
    c->auth_state = CLIENT_AUTH_NONCE;

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

static int make_reply(struct client *c) {
    switch (c->auth_state) {
    case CLIENT_AUTH_NONCE: {
        size_t sig_len;
        unsigned char* signed_nonce = sign_message_pss(private_key, c->in_buf, c->in_len, &sig_len);
        if (signed_nonce == NULL) {
            fprintf(stderr, "Failed to sign nonce\n");
            return -1;
        }
        c->out_len = certificate_len + sig_len + 2*sizeof(uint32_t);
        if (c->out_len > MAX_MESSAGE_SIZE + 4) {
            fprintf(stderr, "Certificate + signed nonce too long\n");
            free(signed_nonce);
            return -1;
        }
        unsigned char* buf = c->out_buf;
        encode_u32_be(buf, (uint32_t)sig_len); buf += sizeof(uint32_t);
        memcpy(buf, signed_nonce, sig_len); buf += sig_len;
        encode_u32_be(buf, (uint32_t)certificate_len); buf += sizeof(uint32_t);
        memcpy(buf, certificate, certificate_len); buf += certificate_len;

        free(signed_nonce);
        c->auth_state = CLIENT_AUTH_SYMKEY;
        c->state = CLIENT_WRITING;
        return 0;
    }
    case CLIENT_AUTH_SYMKEY: {
        size_t shared_key_len;
        unsigned char* shared_key_buf = rsa_decrypt_block(private_key, c->in_buf, c->in_len, &shared_key_len, 1);
        if (shared_key_buf == NULL) {
            return -1;
        }

        if (shared_key_len != SESSION_KEY_LEN) {
            fprintf(stderr, "Cannot parse session key\n");
            free(shared_key_buf);
            return -1;
        }

        memcpy(c->session_key, shared_key_buf, shared_key_len);

        free(shared_key_buf);
        c->auth_state = CLIENT_AUTH_SUCCESS;
        c->state = CLIENT_READING_LEN;
        return 0;
    }
    case CLIENT_AUTH_SUCCESS: {
        htttp_message_t message;
        htttp_make_response(&message, 200, "OK", "Accepted", "text");
        unsigned char* buffer;
        size_t length;
        if (htttp_serialize(&message, &buffer, &length) == -1) {
            perror("serialize");
            return -1;
        }

        size_t out_len;
        unsigned char* out_buffer = session_encrypt(c->session_key, buffer, length, &out_len);

        if (out_buffer == NULL) {
            perror("session_encrypt");
            free(buffer);
            htttp_message_free(&message);
            return -1;
        }

        if (out_len > MAX_MESSAGE_SIZE) {
            free(buffer);
            free(out_buffer);
            fprintf(stderr, "Message too long");
            htttp_message_free(&message);
            return -1;
        }

        encode_u32_be(c->out_buf, (uint32_t)out_len);
        memcpy(c->out_buf + sizeof(uint32_t), out_buffer, out_len);
        c->out_len = sizeof(uint32_t) + out_len;

        free(buffer);
        free(out_buffer);
        htttp_message_free(&message);

        c->state = CLIENT_WRITING;
        break;
    }
    }
    return 0;
}

static int print_client_message(struct client *c) {
    size_t out_len;
    unsigned char* buf = session_decrypt(c->session_key, c->in_buf, c->in_len, &out_len);

    if (buf == NULL) {
        fprintf(stderr, "cannot print message from client %d\n", c->fd);
        return -1;
    }

    printf("client %d sent: %.*s\n", c->fd, (int)out_len, buf);
    fflush(stdout);
    free(buf);
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

            if (c->auth_state == CLIENT_AUTH_SUCCESS) {
                print_client_message(c);
            }

            if (make_reply(c) == -1) {
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

static unsigned char* store_file(const char* path, size_t* len) {
    FILE* cert_file = fopen(path, "rb");
    if (cert_file == NULL) {
        perror("fopen");
        return NULL;
    }
    
    fseek(cert_file, 0, SEEK_END);
    *len = (size_t)ftell(cert_file);
    fseek(cert_file, 0, SEEK_SET);

    unsigned char* cert_buf = malloc(*len);
    if (cert_buf == NULL) {
        fclose(cert_file);
        return cert_buf;
    }

    fread(cert_buf, 1, *len, cert_file);

    fclose(cert_file);

    return cert_buf;
}

static int close_ptr(const int* fd) {
    return close(*fd);
}

static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(EVP_PKEY_free)
static DTOR_WRAPPER_DEFINE(free)

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

    private_key = load_private_key("auth/server_private_key.pem");
    if (private_key == NULL) {
        return -1;
    }
    DTOR_INSERT(dtor, EVP_PKEY_free, private_key);

    certificate = store_file("auth/server_signed.crt", &certificate_len);

    if (certificate == NULL) {
        perror("store certificate");
        return 1;
    }
    DTOR_INSERT(dtor, free, certificate);

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