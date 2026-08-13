#ifndef TETRISH_TETRISD_SERVER_H
#define TETRISH_TETRISD_SERVER_H

#include "acceptor.h"
#include "auth.h"
#include "config_var.h"
#include "control.h"
#include "epoll.h"
#include "htttp_layer.h"
#include "player_io.h"
#include "app/app_layer.h"

typedef struct {
    struct config_var cfg;
    Acceptor acceptor;
    EpollData epoll;
    PlayerIo player_io;
    AuthData auth;
    HtttpData htttp;
    AppData app;
    ControlData control;
} Server;

int server_init(Server* server);
void server_free(Server* server);
void server_tick(Server* server);

/*!
    @brief Validate-then-swap config reload (tetrislogd style), limited to
    directives that apply without reallocating live state. A reload that fails
    to validate leaves the running config untouched.
*/
void server_reload_config(Server* server);

#endif
