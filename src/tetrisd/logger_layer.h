#ifndef TETRISH_TETRISD_LOGGER_LAYER_H
#define TETRISH_TETRISD_LOGGER_LAYER_H

#include "config_var.h"
#include "network/writer.h"
#include <stdbool.h>
#include <stddef.h>

/*!
    @brief The outbound half of the log pipeline: a nonblocking Unix socket to
    tetrislogd, fed by a bounded queue of owned log records.

    Unlike the player-facing layers this one is a singleton, so it carries its
    own fds instead of sparse sets keyed by fd. It also has no @c _reset : the
    queue is a backlog that must survive both ticks and reconnects, like
    PlayerIo's @c write_qs .

    LoggerData_init installs the process log handler
    (@see logger_set_log_handler), which appends to @c queue . Only one
    LoggerData may exist per process, because that handler takes no context
    parameter and the layer therefore reaches the instance through a
    file-static pointer.

    Neither fd is closed here on failure paths that the caller must see first:
    the caller drops the epoll bookkeeping for an fd before this layer closes
    it, since the kernel can hand the same fd number straight back to the next
    accept().

    @invariant At most one of @c fd and @c timerfd is not `-1`. Both are `-1`
    only between a failed reconnect attempt and the next one.
    @invariant Every record in @c queue is a heap-allocated string owned by the
    queue, of length in `[1, FRAME_MAX]`.
*/
typedef struct {
    Writer writer;
    WriterFrameQueue queue;

    // -1 if inactive
    int fd;
    // -1 if inactive
    int timerfd;
    // records discarded since the last drop report, @see LoggerData_write
    size_t dropped;
} LoggerData;

/*!
    @brief allocate the record queue and install the process log handler

    @pre @p data is not initialized
    @pre no other LoggerData is initialized in this process

    @post @c queue has capacity `cfg_var->logger_capacity` and size `0`.
    @post @c fd and @c timerfd are `-1`; the connection is opened by
          LoggerData_accept, not here.
    @post every subsequent LOGGER_LOG appends to @c queue , so this should run
          as early in startup as the configuration allows.

    @return -1 if failed, 0 otherwise
*/
int LoggerData_init(LoggerData* data, struct config_var* cfg_var);

/*!
    @brief flush what can be flushed, then release everything in @p data

    @pre @p data has not been freed
    @pre @c fd and @c timerfd , if active, are no longer registered with epoll

    @post the process log handler is uninstalled before anything else, so the
          teardown itself cannot append to the queue.
    @post queued records are written to @c fd on a bounded best-effort budget;
          whatever does not fit in that budget goes to stderr, which is the
          only destination left once the socket is gone.
    @post @c fd and @c timerfd are closed, the writer and the queue are freed.
*/
void LoggerData_free(LoggerData* data);

/*!
    @brief bring the logger back up: connect, or arm the reconnect timer

    @pre @c fd and @c timerfd are `-1`

    @post on success @c fd is a nonblocking socket whose connect may still be
          in flight. A connect that fails asynchronously is reported later, as
          an epoll hangup or a LoggerData_write failure.
    @post on failure @c timerfd expires once after
          `cfg_var->logger_reconnect_seconds` instead.
    @post if the timer could not be created either, both stay `-1` and the
          caller must retry on a later tick, since nothing will wake it.
    @post the caller registers whichever fd became active with epoll: @c fd
          needs no interest of its own (@see LoggerData_wants_write), @c timerfd
          needs EPOLLIN.
*/
void LoggerData_accept(LoggerData* data, struct config_var* cfg_var);

/*!
    @brief tear the connection down, keeping the undelivered backlog

    @pre @c fd is not `-1`
    @pre @c fd is no longer registered with epoll

    @post @c fd is closed and `-1`; the writer is reset for the next connection.
    @post a partially written record is discarded and counted in @c dropped ;
          records still queued are kept and delivered after the reconnect.
*/
void LoggerData_close(LoggerData* data);

/*!
    @brief arm the reconnect timer on its own

    LoggerData_accept already falls back to this when the connect fails. It is
    also exposed on its own for the caller that rejects a connection the layer
    considers good, the one case being an fd number past the epoll table's
    range: closing it and calling LoggerData_accept again would just produce
    the same out-of-range number, once per tick, forever.

    @pre @c fd and @c timerfd are `-1`

    @post as the failure path of LoggerData_accept.
*/
void LoggerData_arm_timerfd(LoggerData* data, struct config_var* cfg_var);

/*!
    @brief consume the expiration count of a fired reconnect timer

    @pre @c timerfd is not `-1`

    @post the pending expiration is drained, so a level-triggered EPOLLIN on
          @c timerfd does not repeat before it is closed.
*/
void LoggerData_read_timerfd(LoggerData* data);

/*!
    @brief disarm the reconnect timer

    @pre @c timerfd is not `-1`
    @pre @c timerfd is no longer registered with epoll (@see LoggerData_close)

    @post @c timerfd is closed and `-1`.
*/
void LoggerData_close_timerfd(LoggerData* data);

/*!
    @brief report accumulated drops, then drain @p m_write_q to @c fd

    At most a fixed number of records are written per call, so a burst of
    logging cannot starve player I/O; the rest stay queued and epoll reports
    the socket writable again.

    @pre @c fd is not `-1`

    @post records written are freed; records left over stay owned by
          @p m_write_q .
    @post if @c dropped is nonzero and the queue has room, one record naming
          the count is queued and @c dropped is cleared.
    @post on connection failure @c fd is closed and `-1`, as by
          LoggerData_close. The caller detects this by comparing @c fd against
          the value it held before the call, drops the epoll bookkeeping for
          the old fd number, and calls LoggerData_accept.
*/
void LoggerData_write(LoggerData* data, WriterFrameQueue* m_write_q);

/*!
    @brief whether EPOLLOUT should be armed on @c fd

    True while any record is queued or the writer is mid-record. Note that
    EPOLLERR and EPOLLHUP arrive regardless of the interest mask, so a dead
    peer is still noticed with the interest cleared.
*/
bool LoggerData_wants_write(const LoggerData* data);

#endif
