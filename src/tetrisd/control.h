#ifndef TETRISH_TETRISD_CONTROL_H
#define TETRISH_TETRISD_CONTROL_H

#include "type.h"
#include <time.h>

#define CONTROL_MAX_CONNS 2u

typedef enum {
    CONTROL_CONN_READING,
    CONTROL_CONN_RESPONDING,
} ControlConnState;

typedef struct {
    Fd fd; // -1 = slot free
    Reader reader;
    Writer writer;
    ReaderFrameQueue read_q;
    WriterFrameQueue write_q;
    ControlConnState state;
    bool shutdown_on_teardown;
} ControlConn;

/*!
    @brief Inputs Control_process() reads instead of reaching into the other
    layers, so control stays decoupled from them and the call site shows the
    dataflow.
*/
typedef struct {
    size_t players_connected;
    size_t players_authed;
    size_t fds_used;
    size_t fds_capacity;
    int listen_port;
} ControlStatusSnapshot;

/*!
    The admin control plane: a Unix-domain listener plus a fixed array of at
    most CONTROL_MAX_CONNS connections, each serving exactly one framed HTTTP
    request before being closed. A single-purpose component in the sense of
    docs/tetrisd/layers.md: it follows the layer lifecycle naming without the
    per-fd sparse-set structure.

    @invariant A slot in @c conns is live iff its @c fd is not `-1`, and then
    its reader/writer and read_q/write_q are initialized.
    @invariant Connection fds are closed by the epoll layer (like player fds);
    @c listen_fd stays owned here.
*/
typedef struct {
    Fd listen_fd;
    const char* ipc_path; // non-owning view into cfg.control_ipc; cfg outlives this layer
    ControlConn conns[CONTROL_MAX_CONNS];
    Vec_Fd accepted;
    Vec_Fd conns_reading;
    Vec_Fd conns_writing;
    Vec_WriterQueueStatusEntry write_qs_status;
    struct timespec start_time;
} ControlData;

/*!
    @brief Bind and listen on the control socket at @p control_ipc and allocate
    the per-tick collections.

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

    @pre live connection fds have been (or will be) closed by the epoll layer
*/
void Control_free(ControlData* data);

/*!
    @brief tick-end reset of the per-tick vectors; drops read_q leftovers.

    The front frame was consumed in place by Control_process() this tick; any
    frame behind it violates one-request-per-connection and is dropped with it.
*/
void Control_reset(ControlData* data);

/*!
    @brief accept(2) until EAGAIN, initializing a slot per connection and
    pushing its fd to @p m_accepted_out for epoll registration.

    Busy policy: when no slot is free, or the fd does not fit the epoll table
    ( @p fd_capacity ), the connection is closed immediately and the client
    sees EOF.
*/
void Control_accept(ControlData* data, size_t fd_capacity, Vec_Fd* m_accepted_out);

/*!
    @brief One read pass over every fd in @p m_conns_reading , appending
    complete frames to the slot's read_q and marking dead fds in
    @p m_close_fds .

    @note fds already in @p m_close_fds are skipped.
*/
void Control_read(ControlData* data, const Vec_Fd* m_conns_reading, SparseSet_bool* m_close_fds);

/*!
    @brief Serve the front read_q frame of every live CONTROL_CONN_READING
    connection: parse it as HTTTP, route it, and stage exactly one serialized
    response frame into the slot's write_q.

    A shutdown request only takes effect after its response is flushed:
    Control_write() marks the fd for close once drained, and Control_close()
    applies the stop flag (respond-then-act).

    @post a served connection is in CONTROL_CONN_RESPONDING; an operation
    failure (allocation) marks the fd in @p m_close_fds instead.
*/
void Control_process(ControlData* data, const ControlStatusSnapshot* snapshot, SparseSet_bool* m_close_fds);

/*!
    @brief Drain every live connection's write_q (readiness-independent, like
    PlayerIo_write), then either mark the fd in @p m_close_fds (response fully
    flushed, or socket error) or append its queue status to
    @p m_write_qs_status for interest sync.

    @post no fd appears both in @p m_close_fds and in @p m_write_qs_status
    (Epoll_sync_interest precondition)
*/
void Control_write(ControlData* data, SparseSet_bool* m_close_fds, Vec_WriterQueueStatusEntry* m_write_qs_status);

/*!
    @brief Free the slot of every fd in @p m_close_fds , applying a pending
    shutdown_on_teardown (sets @c running to 0).

    @note does not close(2) the fds; Epoll_close() owns that, as for players.
*/
void Control_close(ControlData* data, const SparseSet_bool* m_close_fds);

#endif
