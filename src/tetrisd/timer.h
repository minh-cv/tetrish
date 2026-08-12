#ifndef TETRISH_TETRISD_TIMER_H
#define TETRISH_TETRISD_TIMER_H

#include <stdint.h>

/*!
    @brief create and arm a periodic non-blocking monotonic timerfd

    @pre @p interval_ms is nonzero
    @post on success, the caller uniquely owns a close-on-exec timer descriptor

    @return the descriptor on success, `-1` on invalid interval or syscall error
*/
int periodic_timer_create(uint64_t interval_ms);

/*!
    @brief drain accumulated expirations from a readable timerfd

    @pre @p fd is an initialized periodic timerfd reported readable by epoll
    @post the timerfd counter is drained, including expirations coalesced while busy

    @return the nonzero expiration count on success, `0` on read/error failure
*/
uint64_t periodic_timer_drain(int fd);

#endif
