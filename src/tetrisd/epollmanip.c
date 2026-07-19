#include "epollmanip.h"
#include "client_auth.h"
#include "client_io.h"
#include "client_logger.h"
#include "log_buf.h"
#include "logger.h"
#include "tetris_client.h"
#include "tetrissh.h"

#include <assert.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <unistd.h>

ClientIo* get_client_io(Client* c) {
    switch (c->tag) {
    case CLIENT_TAG_UNAUTHED:
        return &c->client_unauthed.base;
    case CLIENT_TAG_TETRIS:
        return &c->tetris_client.base;
    case CLIENT_TAG_LOGGER:
        return &c->client_logger.base;
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
    case CLIENT_TAG_LOGGER:
        client_logger_free(&c->client_logger);
        return;
    case CLIENT_TAG_INACTIVE:
        assert(false);
        return;
    default:
        assert(false);
        return;
    }
}

static int try_epoll(int epoll_fd, ClientIo* c, int op) {
    uint32_t evs = EPOLLRDHUP;
    if (c->state == CLIENT_READING_LEN || c->state == CLIENT_READING_BODY) {
        evs |= EPOLLIN;
    }
    else if (c->state == CLIENT_WRITING) {
        evs |= EPOLLOUT;
    }
    struct epoll_event ev = {0};
    ev.events = evs;
    ev.data.fd = c->fd;
    return epoll_ctl(epoll_fd, op, c->fd, &ev);
}

static int try_add_epoll(int epoll_fd, ClientIo* c) {
    return try_epoll(epoll_fd, c, EPOLL_CTL_ADD);
}

static int try_mod_epoll(int epoll_fd, ClientIo* c) {
    return try_epoll(epoll_fd, c, EPOLL_CTL_MOD);
}

int add_client(Client* client, int epoll_fd, int client_fd, TetrishCredential* credential) {
    client->tag = CLIENT_TAG_UNAUTHED;
    client_unauthed_init(&client->client_unauthed, client_fd, credential);

    if (try_add_epoll(epoll_fd, get_client_io(client)) == -1) {
        close_client(epoll_fd, client);
        return -1;
    }

    return 0;
}

int add_logger_client(Client* client, int epoll_fd, int client_fd, LogBuf* buf) {
    client->tag = CLIENT_TAG_LOGGER;
    client_logger_init(&client->client_logger, buf, client_fd);

    if (try_add_epoll(epoll_fd, &client->client_logger.base) == -1) {
        close_client(epoll_fd, client);
        return -1;
    }

    return 0;
}

ClientIoResult transist_read_assert_false(struct ClientIo *c_base) {
    (void)c_base;
    assert(false);
    return CLIENT_IO_ERR;
}

enum ClientIoResult client_generic_entry(Client* client) {
    switch (client->tag) {
    case CLIENT_TAG_UNAUTHED:
        return client_io_generic_entry(get_client_io(client), client_unauthed_transist_read, client_unauthed_transit_write);
    case CLIENT_TAG_TETRIS:
        return client_io_generic_entry(get_client_io(client), tetris_client_transist_read, tetris_client_transist_write);
    case CLIENT_TAG_LOGGER:
        return client_io_generic_entry(get_client_io(client), transist_read_assert_false, client_logger_transist_write);
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

ClientIoResult resume_client_event(int epoll_fd, Client* c) {
    for (;;) {
        ClientIo* c_io = get_client_io(c);
        int would_transit = c_io->state == CLIENT_READ_TRANSIT || c_io->state == CLIENT_WRITE_TRANSIT ? 1 : c_io->state == CLIENT_WRITING ? 2 : 3;
        ClientIoResult r = client_generic_entry(c);
        int would_transit_after = c_io->state == CLIENT_READ_TRANSIT || c_io->state == CLIENT_WRITE_TRANSIT ? 1 : c_io->state == CLIENT_WRITING ? 2 : 3;

        if (would_transit != would_transit_after && r != CLIENT_IO_ERR && r != CLIENT_IO_CLOSE) {
            if (try_mod_epoll(epoll_fd, c_io) == -1) {
                r = CLIENT_IO_ERR;
            }
        }

        switch (r) {
            case CLIENT_IO_CONTINUE:
                continue;
            case CLIENT_IO_WOULDBLOCK:
                return r;
            case CLIENT_IO_ERR:
                LOGGER_LOG(LOG_INFO, "client", "client event failure, closing client fd=%d", c_io->fd);
                close_client(epoll_fd, c);
                return r;
            case CLIENT_IO_CLOSE: {
                LOGGER_LOG(LOG_INFO, "client", "closing client fd=%d", c_io->fd);
                close_client(epoll_fd, c);
                return r;
            }
            case CLIENT_IO_YIELD: {
                return r;
            }
        }
    }
}
