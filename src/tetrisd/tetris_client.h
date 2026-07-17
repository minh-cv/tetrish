#ifndef TETRISH_TETRISD_TETRIS_CLIENT_H
#define TETRISH_TETRISD_TETRIS_CLIENT_H

#include "client_io.h"
#include "tetrissh.h"

//! @brief a fully authenticated tetrisd connection - the richer client type that
//! ClientUnauthed is promoted to once the handshake completes (CLIENT_IO_YIELD).
typedef struct TetrisClient {
    struct ClientIo base;
    SessionKey session_key;
} TetrisClient;

//! @brief promote an authenticated ClientUnauthed into a TetrisClient, taking over its
//! fd and session key. Does not touch epoll registration - the fd stays registered as-is.
void tetris_client_init(TetrisClient* c, int client_fd, SessionKey* key);
void tetris_client_free(TetrisClient* c);
ClientIoResult tetris_client_transist_write(int epoll_fd, struct ClientIo* c_base);
ClientIoResult tetris_client_transist_read(int epoll_fd, struct ClientIo* c_base);

#endif
