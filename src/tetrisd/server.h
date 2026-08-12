#ifndef TETRISH_TETRISD_SERVER_H
#define TETRISH_TETRISD_SERVER_H

#include "acceptor.h"
#include "application.h"
#include "auth.h"
#include "config_var.h"
#include "epoll.h"
#include "player_io.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    struct config_var cfg;
    Acceptor acceptor;
    EpollData epoll;
    PlayerIo player_io;
    AuthData auth;
    ServerApplication application;
    int state_timer_fd;
    bool logger_connected;
    uint64_t logger_next_check_ms;
} Server;

int server_init(Server* server);
void server_free(Server* server);
void server_tick(Server* server);

#endif
