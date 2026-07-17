#include "epollmanip.h"
#include "client_auth.h"
#include "client_io.h"
#include "tetris_client.h"
#include "tetrissh.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

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
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = client_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        client->tag = CLIENT_TAG_INACTIVE;
        return -1;
    }

    return 0;
}

enum ClientIoResult client_generic_entry(int epoll_fd, Client* client) {
    switch (client->tag) {
    case CLIENT_TAG_UNAUTHED:
        return client_io_generic_entry(epoll_fd, get_client_io(client), client_unauthed_transist_read, client_unauthed_transit_write);
    case CLIENT_TAG_TETRIS:
        return client_io_generic_entry(epoll_fd, get_client_io(client), tetris_client_transist_read, tetris_client_transist_write);
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

int handle_client_event(int epoll_fd, Client* c) {
    for (;;) {
        ClientIo* c_io = get_client_io(c);
        ClientIoResult r = client_generic_entry(epoll_fd, c);

        switch (r) {
            case CLIENT_IO_CONTINUE:
                continue;
            case CLIENT_IO_OK:
                return 0;
            case CLIENT_IO_ERR:
                printf("client event failure, closing client fd=%d\n", c_io->fd);
                close_client(epoll_fd, c);
                return -1;
            case CLIENT_IO_CLOSE: {
                printf("closing client fd=%d\n", c_io->fd);
                close_client(epoll_fd, c);
                return 0;
            }
            case CLIENT_IO_YIELD: {
                switch (c->tag) {
                case CLIENT_TAG_UNAUTHED:
                ;
                ClientUnauthed old = c->client_unauthed;
                tetris_client_init(&c->tetris_client, old.base.fd, &old.key);
                c->tag = CLIENT_TAG_TETRIS;

                continue;
                default:
                    assert(false);
                    return -1;
                }
            }
        }
    }
}
