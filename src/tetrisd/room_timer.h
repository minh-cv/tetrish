#ifndef TETRISH_TETRISD_ROOM_TIMER_H
#define TETRISH_TETRISD_ROOM_TIMER_H

#include "type.h"
#include <stdint.h>
#include <sys/timerfd.h>

/*!
    The game clock: one timerfd ticking at cfg.room_tick_hz for every room at
    once, since the rate is global and rooms advance in lockstep. A
    single-purpose component in the sense of docs/tetrisd/layers.md — it follows
    the lifecycle naming without the per-fd sparse-set structure, and being a
    singleton it reports readiness through EpollSignals rather than an fd list.

    @invariant @c fd is owned here: the epoll layer closes player and control
    connection fds only, so RoomTimer_free() is what closes this one.
    @invariant @c expirations is per-tick output state, valid from the
    RoomTimer_read() that produced it until RoomTimer_reset() clears it.
*/
typedef struct {
    Fd fd;
    //! @brief the armed interval, kept so reconfig can skip an unchanged rate
    struct itimerspec tick_spec;
    //! @brief ticks elapsed since the last read; > 1 means the loop fell behind
    uint64_t expirations;
} RoomTimer;

/*!
    @brief Create the timerfd and arm it at @p tick_hz .

    @pre @p tick_hz is nonzero and at most one tick per nanosecond (config_var
         enforces both bounds); zero would disarm the timer rather than slow it,
         and a higher rate truncates the period to zero, which disarms it too
    @post the fd is nonblocking and not yet registered with epoll
*/
int RoomTimer_init(RoomTimer* data, unsigned int tick_hz);

/*!
    @brief Close the timerfd.

    @pre the fd has been removed from epoll, or the epoll table is being torn
         down in the same shutdown
*/
void RoomTimer_free(RoomTimer* data);

/*!
    @brief Re-arm at @p tick_hz , a no-op when the rate is unchanged.

    Phase is not preserved across a rate change: the new interval starts from
    the call, so the tick following a reload can be short or long by up to one
    period.

    @pre @p tick_hz is nonzero and at most one tick per nanosecond, as in
         RoomTimer_init()
    @post on failure the previous rate stays armed and the caller keeps the
          running config, so a bad reload cannot stop the clock
    @return `0` on success, `-1` on failure
*/
int RoomTimer_reconfig(RoomTimer* data, unsigned int tick_hz);

/*!
    @brief tick-end reset; clears @c expirations .

    The count is consumed by reading in place, like every other per-tick output
    state, so this is where it is discarded.
*/
void RoomTimer_reset(RoomTimer* data);

/*!
    @brief Read the pending expiration count into @p m_expirations_out .

    A read with nothing pending is not an error: the timerfd is nonblocking, so
    a spurious wakeup yields EAGAIN and a count of `0` rather than a failure.
    The count is not clamped — a loop that overran several periods reports all
    of them, and it is the application layer's to decide whether to advance the
    games that many times or to drop the backlog.

    @post the count is `0` unless this call returned `0`
    @return `0` on success, `-1` if the timerfd is unreadable
*/
int RoomTimer_read(RoomTimer* data, uint64_t* m_expirations_out);

#endif
