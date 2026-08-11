#ifndef TETRISH_TETRISD_SERVER_H
#define TETRISH_TETRISD_SERVER_H

#include "acceptor.h"
#include "auth.h"
#include "config_var.h"
#include "epoll.h"
#include "htttp_layer.h"
#include "logger_layer.h"
#include "player_io.h"
#include "app_layer.h"
#include "ctl.h"

typedef struct {
    struct config_var cfg;
    LoggerData logger;
    Acceptor acceptor;
    EpollData epoll;
    PlayerIo player_io;
    AuthData auth;
    HtttpData htttp;
    AppData app;

    /*
        One process-wide timer at the game frame rate, not one per room: room
        count is bounded by max_rooms but fd count is bounded by max_fds, and
        per-room timers would burn the latter to express the former.
    */
    int room_timerfd;
    uint64_t broadcast_counter;

    CtlData ctl;
    ServerLifecycle lifecycle;
    time_t started_at;
} Server;

int server_init(Server* server);
void server_free(Server* server);
void server_tick(Server* server);

/*!
    @brief whether the loop should end

    True once a shutdown was acknowledged and either the control reply is out
    or the grace deadline passed. The deadline exists because a tetrisctl that
    dies between sending and reading would otherwise wedge the daemon.
*/
bool server_should_stop(const Server* server);

#endif
