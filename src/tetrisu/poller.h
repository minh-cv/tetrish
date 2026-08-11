#ifndef TETRISH_TETRISU_POLLER_H
#define TETRISH_TETRISU_POLLER_H

#include <poll.h>
#include <stdbool.h>

/*!
    @brief The client's event sources.

    Three fixed descriptors do not justify epoll plus a registry, so this is a
    fixed array with a slot per source and an infinite timeout: every tick is
    still one wakeup driven by readiness, exactly as in tetrisd. The frame
    timer is a timerfd rather than a poll timeout so that "tick" keeps meaning
    "something became ready" and no layer has to compute deadlines.
*/
typedef enum {
    POLLER_SLOT_SERVER,
    POLLER_SLOT_INPUT,
    POLLER_SLOT_FRAME_TIMER,
    POLLER_SLOT_COUNT,
} PollerSlot;

typedef struct {
    struct pollfd slots[POLLER_SLOT_COUNT];  // fd < 0 means the slot is unused
    short revents[POLLER_SLOT_COUNT];
} Poller;

void Poller_init(Poller* data);

/*!
    @brief register @p fd in @p slot with interest @p events

    @pre the slot is unused
    @post the slot is readable through Poller_ready until Poller_close
*/
void Poller_accept(Poller* data, PollerSlot slot, int fd, short events);

void Poller_close(Poller* data, PollerSlot slot);

/*!
    @brief block until at least one registered source is ready

    @post @c revents holds this tick's readiness; EINTR yields an all-zero
          tick rather than a fault, so a signal just re-enters the loop
    @return -1 on an unrecoverable poll failure
*/
int Poller_poll(Poller* data);

bool Poller_ready(const Poller* data, PollerSlot slot, short events);

/*!
    @brief level-synchronize interest masks at tick end

    @post POLLOUT on POLLER_SLOT_SERVER is armed iff @p write_pending
    @post POLLIN on POLLER_SLOT_INPUT is disarmed iff @p input_eof
*/
void Poller_sync_interest(Poller* data, bool write_pending, bool input_eof);

/*!
    @post this tick's readiness is cleared
*/
void Poller_reset(Poller* data);

#endif
