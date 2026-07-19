#ifndef TETRISH_TETRISD_EPOLLMANIP_H
#define TETRISH_TETRISD_EPOLLMANIP_H

#include "client_auth.h"
#include "client_logger.h"
#include "tetris_client.h"
#include "tetrissh.h"

typedef struct Client {
    union {
        ClientUnauthed client_unauthed;
        TetrisClient tetris_client;
        ClientLogger client_logger;
    };
    enum {
        CLIENT_TAG_INACTIVE = 0,
        CLIENT_TAG_UNAUTHED,
        CLIENT_TAG_TETRIS,
        CLIENT_TAG_LOGGER,
    } tag;
} Client;

int add_client(Client* client, int epoll_fd, int client_fd, TetrishCredential* credential);
int add_logger_client(Client* client, int epoll_fd, int client_fd, LogBuf* buf);
void close_client(int epoll_fd, Client* c);

/*!
    @brief act as a main entry to handle client event.
*/
ClientIoResult resume_client_event(int epoll_fd, Client* c);
ClientIo* get_client_io(Client* c);

#endif
