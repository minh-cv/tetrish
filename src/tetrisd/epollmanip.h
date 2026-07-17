#ifndef TETRISH_TETRISD_EPOLLMANIP_H
#define TETRISH_TETRISD_EPOLLMANIP_H

#include "client_auth.h"
#include "tetris_client.h"
#include "tetrissh.h"

typedef struct Client {
    union {
        ClientUnauthed client_unauthed;
        TetrisClient tetris_client;
    };
    enum {
        CLIENT_TAG_INACTIVE = 0,
        CLIENT_TAG_UNAUTHED,
        CLIENT_TAG_TETRIS,
    } tag;
} Client;

int add_client(Client* client, int epoll_fd, int client_fd, TetrishCredential* credential);
void close_client(int epoll_fd, Client* c);

//! @brief drains client_io_generic_entry until it would block or (for a still-unauthenticated
//! connection) completes the handshake, in which case *client is promoted in place from
//! ClientUnauthed to TetrisClient (same union storage, same fd). On error or a voluntary close
//! it calls close_client() itself - the caller does not need to (and should not) call it again.
//! @return 0 on success (including promotion, and after a self-initiated close), -1 on error.
int handle_client_event(int epoll_fd, Client* client);

#endif
