#ifndef TETRISH_TETRISD_CONTROL_H
#define TETRISH_TETRISD_CONTROL_H

#include "type.h"

/*!
    @invariant @c shutdown_requested is acted on only once the write queue has
    drained, so a shutdown that could not be acknowledged is not performed.
*/
typedef struct {
    Fd fd; // -1 = no connection
    Reader reader;
    Writer writer;
    ReaderFrameQueue read_q;
    WriterFrameQueue write_q;
    bool shutdown_requested;
} ControlConn;

/*!
    @brief What a served request asks the server to do.

    Control parses and answers; it does not carry out either action itself, so
    reloading and stopping stay the server's to sequence.
*/
typedef struct {
    bool reload_config;
    bool shutdown;
} ControlActions;

/*!
    @brief The interest mask the connection wants, for the caller to hand to
    Epoll_set_interest. @c fd is `-1` when there is nothing to sync.
*/
typedef struct {
    Fd fd;
    EpollInterest interest;
} ControlInterest;

/*!
    The admin control plane: a Unix-domain listener plus at most one connection,
    which is read like a player's — any number of framed HTTTP requests, one
    response each, until the peer closes. A single-purpose component in the
    sense of docs/tetrisd/layers.md: it follows the layer lifecycle naming
    without the per-fd sparse-set structure, and since it holds one connection
    it reports readiness through EpollSignals rather than through fd lists.

    @invariant @c conn is live iff its @c fd is not `-1`, and then its
    reader/writer and read_q/write_q are initialized.
    @invariant The connection fd is closed by the epoll layer (like player fds);
    @c listen_fd stays owned here.
*/
typedef struct {
    Fd listen_fd;
    const char* ipc_path; // non-owning view into cfg.control_ipc; cfg outlives this layer
    ControlConn conn;
} ControlData;

/*!
    @brief Bind and listen on the control socket at @p control_ipc .

    A stale socket file from a previous run is unlinked first. The socket file
    is created with mode 0600 regardless of the process umask (daemonization
    runs umask(0)): its permissions are the control plane's entire
    authentication boundary.

    @pre @p control_ipc outlives @p data (it is stored, not copied)
    @post the listener is nonblocking and not yet registered with epoll
*/
int Control_init(ControlData* data, const char* control_ipc);

/*!
    @brief Release all resources and unlink the socket path.

    @pre a live connection fd has been (or will be) closed by the epoll layer
*/
void Control_free(ControlData* data);

/*!
    @brief tick-end reset; reclaims the read_q.

    Control_process() consumes its frames in place rather than popping them, so
    this is where they are freed.
*/
void Control_reset(ControlData* data);

/*!
    @brief accept(2) until EAGAIN, keeping at most one connection.

    Admission can fail, and nothing in the kernel guarantees otherwise: accept
    hands back the lowest free fd number, which is only below @p fd_capacity
    (cfg.max_fds) while enough of the table is unused. The gap between
    cfg.max_player_fd and cfg.max_fds is what reserves that room, and it is the
    operator's to configure — set them equal and control accepts fail as soon
    as players fill the table.

    Every non-admission closes the incoming fd immediately, so that client sees
    EOF: a connection is already held, the fd is out of table range, or the
    slot could not be initialized.

    @post an admitted connection has both queues at capacity 1, so it carries
          one request in flight per tick and Control_process's response always
          fits.

    @return the accepted fd for the caller to register with epoll, or `-1` if
            no connection was admitted this call
*/
Fd Control_accept(ControlData* data, size_t fd_capacity);

/*!
    @brief mark the connection for close after an EPOLLERR/EPOLLHUP

    @note a no-op when no connection is held.
*/
void Control_hangup(ControlData* data, SparseSet_bool* m_close_fds);

/*!
    @brief whether a request is waiting to be served

    Lets the caller skip rendering the state dump on ticks with no request.
*/
bool Control_has_request(const ControlData* data);

/*!
    @brief One read pass over the live connection, appending complete frames to
    its read_q and marking a dead fd in @p m_close_fds .

    EOF fails the fd, as it does for players: a peer that closes mid-request
    loses whatever it had queued.

    @note an fd already in @p m_close_fds is skipped.
*/
void Control_read(ControlData* data, SparseSet_bool* m_close_fds);

/*!
    @brief Serve every frame in read_q: parse it as HTTTP, route it, and stage
    one serialized response per request into write_q.

    @p state_json is the server's rendered state dump, used verbatim as the
    body of a GET /status. A shutdown request is reported through
    @p m_actions only once its response has flushed, which Control_write
    determines (respond-then-act); a reload is reported as soon as it is
    answered.

    @pre read_q and write_q have the same capacity, so the
         one-response-per-request output always fits what has not yet drained
    @pre @p state_json is non-empty if a request could be a GET /status
    @post an operation failure (allocation, a missing state dump, or a write_q
          that the previous tick's backlog left full) marks the fd in
          @p m_close_fds
*/
void Control_process(ControlData* data, const char* state_json, size_t state_json_len,
                     SparseSet_bool* m_close_fds, ControlActions* m_actions);

/*!
    @brief Drain the connection's write_q (readiness-independent, like
    PlayerIo_write).

    @post a socket failure marks the fd in @p m_close_fds ; a queue that
          drained with a shutdown pending sets @c shutdown in @p m_actions .
    @post @p m_interest_out carries the mask to apply, or `fd == -1` when the
          connection is gone or closing — so the caller never has to inspect
          this layer's internals to sync epoll.
*/
void Control_write(ControlData* data, SparseSet_bool* m_close_fds,
                   ControlInterest* m_interest_out, ControlActions* m_actions);

/*!
    @brief Free the connection if its fd is in @p m_close_fds .

    @note does not close(2) the fd; Epoll_close() owns that, as for players.
*/
void Control_close(ControlData* data, const SparseSet_bool* m_close_fds);

#endif
