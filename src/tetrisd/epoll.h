#ifndef TETRISH_TETRISD_EPOLL_H
#define TETRISH_TETRISD_EPOLL_H

#include "type.h"
#include <sys/epoll.h>

typedef enum {
    EPOLL_ENTRY_INACTIVE,
    EPOLL_ENTRY_PLAYER,
    EPOLL_ENTRY_ROOM_TIMERFD,
    EPOLL_ENTRY_LOGGER_TIMERFD,
    EPOLL_ENTRY_LOGGER,
    EPOLL_ENTRY_CONTROL,
    EPOLL_ENTRY_ACCEPTOR,
} EpollEntryType;

typedef struct {
    EpollEntryType type;
    EpollInterest current_interest;
} EpollEntry;

#define SPARSE_SET_ELEM_TYPE EpollEntry
#define SPARSE_SET_TYPEDEF SparseSet_EpollEntry
#include "collection/sparse_set.h"

#define SPAN_ELEM_TYPE struct epoll_event
#define SPAN_TYPEDEF EpollEvents
#include "collection/span.h"

typedef struct {
    SparseSet_EpollEntry entries;
    EpollEvents events;
    int epoll_fd;
    SparseSet_bool player_close_fds;
} EpollData;

// takes ownership of epoll_fd; player fds are closed by Epoll_close/Epoll_free,
// non-player fds (acceptor, ...) stay owned by their layer.
int Epoll_init(
    EpollData* data,
    size_t max_entries,
    size_t max_events,
    int epoll_fd
);

void Epoll_free(
    EpollData* data
);

// tick-end reset of the close accumulator; must run after every close fan-out
void Epoll_reset(
    EpollData* data
);

void Epoll_accept(
    EpollData* data,
    const Vec_Fd* fds,
    EpollEntryType type,
    EpollInterest interest,
    SparseSet_bool* m_err_fds
);

// single-fd registration for singleton entries (acceptor, logger, timerfds)
int Epoll_accept_one(
    EpollData* data,
    Fd fd,
    EpollEntryType type,
    EpollInterest interest
);

void Epoll_close(
    EpollData* data,
    const SparseSet_bool* m_close_fds
);

int Epoll_poll(
    EpollData* data,
    Vec_Fd* player_read,
    Vec_Fd* player_write,
    bool* acceptor_readable,
    bool* state_timer_readable
);

/*!
    Level synchronization of interest masks. Players: EPOLLOUT armed iff the
    entry's status is not WRITER_QUEUE_EMPTY, visiting only the fds listed in
    write_qs_status — PlayerIo_write's postcondition guarantees that list covers
    exactly the fds whose queue level could have changed this tick, there is no
    full rescan. Acceptor: EPOLLIN disarmed when no more connections can be
    admitted (should_stop_accepting), rearmed once a close frees a slot.

    @pre No fd in @p write_qs_status is in @p m_close_fds
*/
void Epoll_sync_interest(
    EpollData* data,
    const Vec_WriterQueueStatusEntry* write_qs_status,
    const SparseSet_bool* m_close_fds,
    Fd acceptor_fd,
    bool should_stop_accepting
);

#endif
