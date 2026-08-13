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
    @brief Inputs Control_process() reads instead of reaching into the other
    layers, so control stays decoupled from them and the call site shows the
    dataflow.
*/
typedef struct {
    size_t players_connected;
    size_t players_authed;
    size_t players_capacity;
    size_t fds_used;
    size_t fds_capacity;
    int listen_port;
} ControlStatusSnapshot;

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

    A shutdown request only takes effect after its response is flushed:
    Control_write() acts on @c shutdown_requested once the queue drains
    (respond-then-act).

    @pre read_q and write_q have the same capacity, so the
         one-response-per-request output always fits what has not yet drained
    @post an operation failure (allocation, or a write_q that the previous
          tick's backlog left full) marks the fd in @p m_close_fds
*/
void Control_process(ControlData* data, const ControlStatusSnapshot* snapshot, SparseSet_bool* m_close_fds);

/*!
    @brief Drain the connection's write_q (readiness-independent, like
    PlayerIo_write).

    @post a socket failure marks the fd in @p m_close_fds ; a queue that
          drained with @c shutdown_requested set stops the main loop.
*/
void Control_write(ControlData* data, SparseSet_bool* m_close_fds);

/*!
    @brief whether EPOLLOUT should be armed on the connection
*/
bool Control_wants_write(const ControlData* data);

/*!
    @brief Free the connection if its fd is in @p m_close_fds .

    @note does not close(2) the fd; Epoll_close() owns that, as for players.
*/
void Control_close(ControlData* data, const SparseSet_bool* m_close_fds);

#endif
