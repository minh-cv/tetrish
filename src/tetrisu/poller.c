#include "poller.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

void Poller_init(Poller* data) {
    for (size_t i = 0; i < POLLER_SLOT_COUNT; i++) {
        data->slots[i].fd = -1;
        data->slots[i].events = 0;
        data->slots[i].revents = 0;
        data->revents[i] = 0;
    }
}

void Poller_accept(Poller* data, PollerSlot slot, int fd, short events) {
    assert(data->slots[slot].fd == -1 && "slot already in use");
    data->slots[slot].fd = fd;
    data->slots[slot].events = events;
    data->slots[slot].revents = 0;
    data->revents[slot] = 0;
}

void Poller_close(Poller* data, PollerSlot slot) {
    data->slots[slot].fd = -1;
    data->slots[slot].events = 0;
    data->slots[slot].revents = 0;
    data->revents[slot] = 0;
}

int Poller_poll(Poller* data) {
    /*
        poll() skips a negative fd, so unused slots need no compaction and the
        slot index stays the identity of a source across ticks.
    */
    const int ready = poll(data->slots, POLLER_SLOT_COUNT, -1);
    if (ready == -1) {
        if (errno == EINTR) {
            // a signal, not a failure: the caller re-enters with nothing ready
            return 0;
        }
        perror("poll");
        return -1;
    }

    for (size_t i = 0; i < POLLER_SLOT_COUNT; i++) {
        data->revents[i] = data->slots[i].revents;
    }
    return 0;
}

bool Poller_ready(const Poller* data, PollerSlot slot, short events) {
    return (data->revents[slot] & events) != 0;
}

void Poller_sync_interest(Poller* data, bool write_pending, bool input_eof) {
    if (data->slots[POLLER_SLOT_SERVER].fd != -1) {
        data->slots[POLLER_SLOT_SERVER].events =
            write_pending ? (short)(POLLIN | POLLOUT) : (short)POLLIN;
    }
    if (data->slots[POLLER_SLOT_INPUT].fd != -1) {
        data->slots[POLLER_SLOT_INPUT].events = input_eof ? (short)0 : (short)POLLIN;
    }
}

void Poller_reset(Poller* data) {
    memset(data->revents, 0, sizeof(data->revents));
}
