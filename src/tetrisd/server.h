#ifndef TETRISH_TETRISD_SERVER_H
#define TETRISH_TETRISD_SERVER_H

#include "acceptor.h"
#include "auth.h"
#include "config_var.h"
#include "control.h"
#include "epoll.h"
#include "htttp_layer.h"
#include "logger_layer.h"
#include "player_io.h"
#include "app/app_layer.h"

typedef struct {
    struct config_var cfg;
    LoggerData logger;
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

/*!
    @brief One iteration of the event loop, signal handling included.

    The pending signal flags are consumed at the top, before the poll: SIGTERM
    and SIGINT stop the loop, SIGHUP reloads the configuration, and SIGUSR1
    dumps the server state to the log. A signal that interrupts epoll_wait
    leaves the flag for the next call rather than being reported as a stop.

    @return `0` to continue, `-1` once the daemon should stop — either from a
            termination signal or from a control shutdown whose response has
            already been flushed.
*/
int server_tick(Server* server);

/*!
    @brief Validate-then-swap config reload (tetrislogd style), limited to
    directives that apply without reallocating live state. A reload that fails
    to validate leaves the running config untouched.
*/
void server_reload_config(Server* server);

#endif
