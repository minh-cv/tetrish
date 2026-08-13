#include "epoll.h"
#include "dtor.h"
#include "logger.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(SparseSet_EpollEntry_free)
static DTOR_WRAPPER_DEFINE(SparseSet_bool_free)

int Epoll_init(EpollData* data, size_t max_entries, size_t max_events, int epoll_fd) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (SparseSet_EpollEntry_init(&data->entries, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_EpollEntry_free, &data->entries);

    if (EpollEvents_calloc(&data->events, max_events) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, data->events.ptr);

    if (SparseSet_bool_init(&data->player_close_fds, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_bool_free, &data->player_close_fds);

    if (SparseSet_bool_init(&data->control_close_fds, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    data->epoll_fd = epoll_fd;
    DTOR_RETURN(dtor, 0);
}

void Epoll_free(EpollData* data) {
    for (size_t i = 0; i < SparseSet_EpollEntry_size(&data->entries); i++) {
        const EpollEntryType type = SparseSet_EpollEntry_at_idx(&data->entries, i)->type;
        if (type == EPOLL_ENTRY_PLAYER || type == EPOLL_ENTRY_CONTROL) {
            close((int)SparseSet_EpollEntry_key_at_idx(&data->entries, i));
        }
    }
    close(data->epoll_fd);
    data->epoll_fd = -1;
    SparseSet_bool_free(&data->control_close_fds);
    SparseSet_bool_free(&data->player_close_fds);
    free(data->events.ptr);
    SparseSet_EpollEntry_free(&data->entries);
}

void Epoll_reset(EpollData* data) {
    SparseSet_bool_reset(&data->player_close_fds);
    SparseSet_bool_reset(&data->control_close_fds);
}

int Epoll_accept_one(EpollData* data, Fd fd, EpollEntryType type, EpollInterest interest) {
    assert(fd >= 0 && (size_t)fd < data->entries.capacity);
    assert(!SparseSet_EpollEntry_contains(&data->entries, (size_t)fd));

    // ctl before insert so a ctl failure leaves the set untouched
    struct epoll_event ev = {0};
    ev.events = interest;
    ev.data.fd = fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        LOGGER_PERROR("epoll", "epoll_ctl add");
        return -1;
    }

    const EpollEntry entry = { type, interest };
    const int err = SparseSet_EpollEntry_insert(&data->entries, (size_t)fd, &entry);
    assert(err != -1);
    (void)err;
    return 0;
}

void Epoll_accept(EpollData* data, const Vec_Fd* fds, EpollEntryType type, EpollInterest interest, SparseSet_bool* m_err_fds) {
    for (size_t i = 0; i < Vec_Fd_size(fds); i++) {
        const Fd fd = *Vec_Fd_at(fds, i);
        if (Epoll_accept_one(data, fd, type, interest) == -1) {
            *SparseSet_bool_activate(m_err_fds, (size_t)fd) = true;
        }
    }
}

void Epoll_close(EpollData* data, const SparseSet_bool* m_close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(m_close_fds); i++) {
        const size_t fd = SparseSet_bool_key_at_idx(m_close_fds, i);
        // an fd whose registration failed has no entry but is still open
        close((int)fd);
        if (SparseSet_EpollEntry_contains(&data->entries, fd)) {
            // only the fd-owning entry types may appear in a close set
            const EpollEntryType type = SparseSet_EpollEntry_get(&data->entries, fd)->type;
            assert(type == EPOLL_ENTRY_PLAYER || type == EPOLL_ENTRY_CONTROL);
            (void)type;
            SparseSet_EpollEntry_erase(&data->entries, fd);
        }
    }
}

void Epoll_erase_one(EpollData* data, Fd fd) {
    assert(fd >= 0 && (size_t)fd < data->entries.capacity);
    assert(SparseSet_EpollEntry_contains(&data->entries, (size_t)fd));
    SparseSet_EpollEntry_erase(&data->entries, (size_t)fd);
}

int Epoll_poll(EpollData* data, Vec_Fd* player_read, Vec_Fd* player_write, EpollSignals* m_signals) {
    const int n = epoll_wait(data->epoll_fd, data->events.ptr, (int)data->events.length, -1);
    if (n == -1) {
        if (errno != EINTR) {
            LOGGER_PERROR("epoll", "epoll_wait");
        }
        return -1;
    }

    for (size_t i = 0; i < (size_t)n; i++) {
        const struct epoll_event* ev = EpollEvents_at(&data->events, i);
        const Fd fd = ev->data.fd;
        const EpollEntry* entry = SparseSet_EpollEntry_get(&data->entries, (size_t)fd);

        switch (entry->type) {
        case EPOLL_ENTRY_ACCEPTOR:
            if (ev->events & EPOLLIN) {
                m_signals->acceptor_readable = true;
            }
            break;
        case EPOLL_ENTRY_LOGGER:
            if (ev->events & (EPOLLERR | EPOLLHUP)) {
                m_signals->logger_hangup = true;
                break;
            }
            if (ev->events & EPOLLOUT) {
                m_signals->logger_writable = true;
            }
            break;
        case EPOLL_ENTRY_LOGGER_TIMERFD:
            if (ev->events & EPOLLIN) {
                m_signals->logger_timer_expired = true;
            }
            break;
        case EPOLL_ENTRY_ROOM_TIMERFD:
            if (ev->events & EPOLLIN) {
                m_signals->room_timer_expired = true;
            }
            break;
        case EPOLL_ENTRY_GARBAGE_MQ:
            // EPOLLERR too: the drain must run to observe and report the error
            if (ev->events & (EPOLLIN | EPOLLERR)) {
                m_signals->garbage_readable = true;
            }
            break;
        case EPOLL_ENTRY_PLAYER:
            // EPOLLIN first: a peer that sends and closes reports both at once,
            // and the pending bytes are still worth reading. The reader sees
            // EOF on its next pass and fails the fd then.
            if (ev->events & EPOLLERR) {
                *SparseSet_bool_activate(&data->player_close_fds, (size_t)fd) = true;
                break;
            }
            if (ev->events & EPOLLIN) {
                const int err = Vec_Fd_push_back(player_read, &fd);
                assert(err != -1);
                (void)err;
            }
            else if (ev->events & EPOLLHUP) {
                *SparseSet_bool_activate(&data->player_close_fds, (size_t)fd) = true;
                break;
            }
            if (ev->events & EPOLLOUT) {
                const int err = Vec_Fd_push_back(player_write, &fd);
                assert(err != -1);
                (void)err;
            }
            break;
        case EPOLL_ENTRY_CONTROL_LISTENER:
            if (ev->events & EPOLLIN) {
                m_signals->control_listener_readable = true;
            }
            break;
        case EPOLL_ENTRY_CONTROL:
            if (ev->events & EPOLLERR) {
                m_signals->control_hangup = true;
                break;
            }
            if (ev->events & EPOLLIN) {
                m_signals->control_readable = true;
            }
            else if (ev->events & EPOLLHUP) {
                m_signals->control_hangup = true;
                break;
            }
            if (ev->events & EPOLLOUT) {
                m_signals->control_writable = true;
            }
            break;
        default:
            assert(false);
            break;
        }
    }
    return 0;
}

static void sync_interest_one(EpollData* data, size_t fd, EpollEntry* entry, EpollInterest want) {
    if (want == entry->current_interest) {
        return;
    }

    struct epoll_event ev = {0};
    ev.events = want;
    ev.data.fd = (int)fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_MOD, (int)fd, &ev) == -1) {
        // MOD only fails on programming errors (EBADF/ENOENT/EINVAL/EPERM),
        // never from client behavior.
        LOGGER_PERROR("epoll", "epoll_ctl mod");
        assert(false);
        return;
    }
    entry->current_interest = want;
}

void Epoll_set_interest(EpollData* data, Fd fd, EpollInterest interest) {
    EpollEntry* entry = SparseSet_EpollEntry_get(&data->entries, (size_t)fd);
    assert(entry->type != EPOLL_ENTRY_PLAYER && "players go through Epoll_sync_interest");
    sync_interest_one(data, (size_t)fd, entry, interest);
}

void Epoll_sync_interest(EpollData* data, const Vec_WriterQueueStatusEntry* write_qs_status, const SparseSet_bool* m_close_fds, Fd acceptor_fd, bool should_stop_accepting) {
    for (size_t i = 0; i < Vec_WriterQueueStatusEntry_size(write_qs_status); i++) {
        const WriterQueueStatusEntry* status = Vec_WriterQueueStatusEntry_at(write_qs_status, i);
        const size_t fd = (size_t)status->fd;
        assert(!SparseSet_bool_contains(m_close_fds, fd));
        EpollEntry* entry = SparseSet_EpollEntry_get(&data->entries, fd);
        assert(entry->type == EPOLL_ENTRY_PLAYER);
        const EpollInterest want =
            EPOLLIN | (status->status == WRITER_QUEUE_EMPTY ? 0u : (EpollInterest)EPOLLOUT);
        sync_interest_one(data, fd, entry, want);
    }

    EpollEntry* acceptor = SparseSet_EpollEntry_get(&data->entries, (size_t)acceptor_fd);
    assert(acceptor->type == EPOLL_ENTRY_ACCEPTOR);
    EpollInterest want = acceptor->current_interest;
    if (SparseSet_bool_size(m_close_fds) > 0) {
        want = EPOLLIN;
    }
    else if (should_stop_accepting) {
        want = 0;
    }
    sync_interest_one(data, (size_t)acceptor_fd, acceptor, want);
}
