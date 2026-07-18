#include "epollmanip.h"
#include "client_auth.h"
#include "client_io.h"
#include "logger.h"
#include "tetris_client.h"
#include "tetrissh.h"

#include <assert.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <unistd.h>

static int mod_epoll_events(int epoll_fd, int fd, uint32_t events) {
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}


static ClientIo* get_client_io(Client* c) {
    switch (c->tag) {
    case CLIENT_TAG_UNAUTHED:
        return &c->client_unauthed.base;
    case CLIENT_TAG_TETRIS:
        return &c->tetris_client.base;
    case CLIENT_TAG_INACTIVE:
    default:
        assert(false);
        return NULL;
    }
}

static void client_free(Client* c) {
    switch (c->tag) {
    case CLIENT_TAG_UNAUTHED:
        client_unauthed_free(&c->client_unauthed);
        return;
    case CLIENT_TAG_TETRIS:
        tetris_client_free(&c->tetris_client);
        return;
    case CLIENT_TAG_INACTIVE:
        assert(false);
        return;
    default:
        assert(false);
        return;
    }
}

int add_client(Client* client, int epoll_fd, int client_fd, TetrishCredential* credential) {
    client->tag = CLIENT_TAG_UNAUTHED;
    client_unauthed_init(&client->client_unauthed, client_fd, credential);

    struct epoll_event ev = {0};
    ev.events = EPOLLRDHUP;
    ev.data.fd = client_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        client->tag = CLIENT_TAG_INACTIVE;
        return -1;
    }

    return 0;
}

enum ClientIoResult client_generic_entry(Client* client) {
    switch (client->tag) {
    case CLIENT_TAG_UNAUTHED:
        return client_io_generic_entry(get_client_io(client), client_unauthed_transist_read, client_unauthed_transit_write);
    case CLIENT_TAG_TETRIS:
        return client_io_generic_entry(get_client_io(client), tetris_client_transist_read, tetris_client_transist_write);
    case CLIENT_TAG_INACTIVE:
        assert(false);
        return CLIENT_IO_ERR;
    default:
        assert(false);
        return CLIENT_IO_ERR;
    }
}

void close_client(int epoll_fd, Client* c) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, get_client_io(c)->fd, NULL);
    close(get_client_io(c)->fd);
    client_free(c);
    c->tag = CLIENT_TAG_INACTIVE;
}

static int try_mod_epoll(int epoll_fd, ClientIo* c) {
    if (c->state == CLIENT_READING_LEN || c->state == CLIENT_READING_BODY) {
        if (mod_epoll_events(epoll_fd, c->fd, EPOLLIN | EPOLLRDHUP) == -1) {
            return -1;
        }
    }
    else if (c->state == CLIENT_WRITING) {
        if (mod_epoll_events(epoll_fd, c->fd, EPOLLOUT | EPOLLRDHUP) == -1) {
            return -1;
        }
    }
    return 0;
}

int handle_client_event(int epoll_fd, Client* c) {
    for (;;) {
        ClientIo* c_io = get_client_io(c);
        int would_transit = c_io->state == CLIENT_READ_TRANSIT || c_io->state == CLIENT_WRITE_TRANSIT ? 1 : c_io->state == CLIENT_WRITING ? 2 : 3;
        ClientIoResult r = client_generic_entry(c);
        int would_transit_after = c_io->state == CLIENT_READ_TRANSIT || c_io->state == CLIENT_WRITE_TRANSIT ? 1 : c_io->state == CLIENT_WRITING ? 2 : 3;

        if (would_transit != would_transit_after && (r == CLIENT_IO_WOULDBLOCK || r == CLIENT_IO_CONTINUE)) {
            if (try_mod_epoll(epoll_fd, c_io) == -1) {
                r = CLIENT_IO_ERR;
            }
        }

        switch (r) {
            case CLIENT_IO_CONTINUE:
                continue;
            case CLIENT_IO_WOULDBLOCK:
                return 0;
            case CLIENT_IO_ERR:
                LOGGER_LOG(LOG_INFO, "client", "client event failure, closing client fd=%d", c_io->fd);
                close_client(epoll_fd, c);
                return -1;
            case CLIENT_IO_CLOSE: {
                LOGGER_LOG(LOG_INFO, "client", "closing client fd=%d", c_io->fd);
                close_client(epoll_fd, c);
                return 0;
            }
            case CLIENT_IO_YIELD: {
                switch (c->tag) {
                case CLIENT_TAG_UNAUTHED:{ 
                    ClientUnauthed old = c->client_unauthed;
                    tetris_client_init(&c->tetris_client, old.base.fd, &old.key);
                    c->tag = CLIENT_TAG_TETRIS;
                    continue;
                }
                default:
                    assert(false);
                    return -1;
                }
            }
        }
    }
}
