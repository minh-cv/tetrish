#ifndef TETRISH_TETRISD_CTL_H
#define TETRISH_TETRISD_CTL_H

#include "type.h"
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

/*!
    @brief What the daemon is doing about shutting down.

    DRAINING stops admitting players and leaves everything else alone;
    STOPPING means a shutdown was acknowledged and the process exits once the
    control reply is out or the grace deadline passes.
*/
typedef enum {
    SERVER_RUNNING,
    SERVER_DRAINING,
    SERVER_STOPPING,
} ServerLifecycle;

/*!
    @brief A flat read-only snapshot of the daemon, built once per control
    request.

    The responder takes this rather than a back-pointer to Server, so the
    layer graph stays acyclic and answering a status query is testable without
    a running daemon.
*/
typedef struct {
    pid_t pid;
    ServerLifecycle lifecycle;
    const char* address;
    int port;
    long uptime_seconds;
    size_t players_connected;
    size_t players_authenticated;
    size_t rooms;
    size_t max_fds;
    size_t tick_hz;
    bool accepting;
} ServerStatus;

/*!
    @brief The control plane: an AF_UNIX listener that speaks HTTTP.

    Availability under flood is the requirement that shapes this. The listener
    is registered in the player epoll so an arriving control connection wakes
    an idle daemon, *and* it is drained unconditionally at the top of every
    tick, so a full event array of player fds cannot starve it. Reserving fds
    for it (@c reserved_fds) keeps a saturated player table from consuming the
    last slots the control accept would need.

    A control connection is served synchronously and completely — accept, read
    one request, answer, close — rather than through a second copy of the
    player pipeline. That is sound only because the channel is local and
    restricted: the socket is chmod 0600 and every peer's uid is checked, and
    both directions carry a short timeout so a wedged peer cannot hold the
    tick.
*/
typedef struct {
    int listen_fd;      // -1 when the control plane is not up
    char* path;         // owned; unlinked at free
    unsigned timeout_ms;

    bool shutdown_requested;
    bool drain_requested;
} CtlData;

/*!
    @brief bind and listen on @p path

    A pre-existing socket file is probed with a connect before being replaced:
    ECONNREFUSED means it is stale and is unlinked, while a successful connect
    means another daemon owns it and is a fatal init error. A blind unlink
    would let a second daemon steal a live daemon's control channel.

    @post on success @c listen_fd is a nonblocking listener whose path is mode
          0600
    @return -1 on failure, with nothing left bound
*/
int CtlData_init(CtlData* data, const char* path, unsigned timeout_ms);

/*!
    @post the listener is closed and its path unlinked
*/
void CtlData_free(CtlData* data);

/*!
    @brief accept and answer every control connection that is waiting

    @post at most @p max_connections are served in one call, so a flood of
          control connections cannot hold the tick either
    @post @c shutdown_requested and @c drain_requested reflect what was asked;
          the caller applies them after the pass, which keeps the effect of a
          command out of the middle of the tick that read it
*/
void CtlData_serve(CtlData* data, const ServerStatus* status, size_t max_connections);

#endif
