#ifndef TETRISH_TETRISD_GARBAGE_H
#define TETRISH_TETRISD_GARBAGE_H

#include "app/garbage_event.h"
#include "config_var.h"
#include <mqueue.h>
#include <stdint.h>

/*!
    The garbage queue: a POSIX message queue carrying each tick's
    @c GarbageEvent s, the required real-IPC leg of battle-royale garbage
    transfer. The daemon is both producer and consumer; what makes it IPC
    is the kernel object in between, never a call into another room's
    state. A singleton component in the sense of docs/tetrisd/layers.md,
    in the @c RoomTimer mold: it reports readiness through
    @c EpollSignals , and an event sent in one tick is drained on a later
    one, when the poller reports the queue readable.

    @invariant @c mq is owned here: the epoll layer closes player and
    control connection fds only, so GarbageData_free() is what closes and
    unlinks it.
    @invariant @c received is per-tick output state, valid from the
    GarbageData_receive() that filled it until GarbageData_reset().
*/
typedef struct {
    //! @brief `-1` when closed; on Linux, pollable like any fd
    mqd_t mq;
    //! @brief points into cfg, like @c ControlData.ipc_path
    const char* mq_name;
    //! @brief the queue's actual message size; bounds @c recv_buf
    long mq_msgsize;
    unsigned char* recv_buf;
    //! @brief stamped onto outgoing events for log correlation
    uint32_t seq;
    Vec_GarbageEvent received;

    // counters, surfaced by /status; dropped_full is reported in aggregate
    uint64_t sent;
    uint64_t dropped_full;
    uint64_t dropped_bad_event;
    uint64_t reported_dropped_full;
} GarbageData;

/*!
    @brief Unlink any stale queue from a previous run, then create ours.

    The queue is opened `O_CREAT | O_EXCL`, so a second daemon instance
    fails loudly instead of sharing it, and `O_NONBLOCK`, so neither
    sending nor draining can ever stall the tick. Its mode is 0600 under a
    scoped umask (incantation() runs `umask(0)` ), matching the control
    socket: same-user tools can open it, other users cannot inject
    garbage. A configured depth the kernel rejects (its unprivileged cap
    is /proc/sys/fs/mqueue/msg_max) falls back to the kernel defaults with
    a warning; the actual attributes are read back and size @c received
    and @c recv_buf .

    @post the queue exists, is empty, and is not yet registered with epoll
    @return -1 on failure — fatal to the caller, like the room timer: a
            spec-required subsystem that cannot start is a configuration
            error
*/
int GarbageData_init(GarbageData* data, const struct config_var* cfg);

/*!
    @brief Close and unlink the queue.

    @pre the mqd has been removed from epoll, or the epoll table is being
         torn down in the same shutdown
*/
void GarbageData_free(GarbageData* data);

/*!
    @brief Send each event of @p events , stamped with a fresh @c seq .

    A full queue drops the event ( @c dropped_full ) rather than blocking
    the tick; the losses are reported in one aggregated warning on a later
    call, the logger's own drop-report shape, never one line per event.
*/
void GarbageData_send(GarbageData* data, const Vec_GarbageEvent* events);

/*!
    @brief Drain the queue into @p m_received until it would block or
           @p m_received is full.

    A message of the wrong size or an unknown version is counted in
    @c dropped_bad_event and skipped: the queue survives a stray writer.
    Anything left undrained keeps the mqd readable, so the next poll
    reports it again.

    @return -1 if the queue is unreadable — fatal to the caller, like an
            unreadable game clock
*/
int GarbageData_receive(GarbageData* data, Vec_GarbageEvent* m_received);

//! @brief tick-end reset; clears @c received
void GarbageData_reset(GarbageData* data);

#endif
