#ifndef TETRISH_TETRISD_SERVER_H
#define TETRISH_TETRISD_SERVER_H

#include "acceptor.h"
#include "auth.h"
#include "config_var.h"
#include "epoll.h"
#include "htttp_layer.h"
#include "player_io.h"
#include "app_layer.h"

typedef struct {
    struct config_var cfg;
    Acceptor acceptor;
    EpollData epoll;
    PlayerIo player_io;
    AuthData auth;
    HtttpData htttp;
    AppData app;
} Server;

int server_init(Server* server);
void server_free(Server* server);
void server_tick(Server* server);

#endif
