#include "player_io.h"
#include "dtor.h"
#include <assert.h>
#include <stdlib.h>

static DTOR_WRAPPER_DEFINE(SparseSet_PlayerIoEntry_free)
static DTOR_WRAPPER_DEFINE(SparseSet_WriterFrameQueue_free)
static DTOR_WRAPPER_DEFINE(SparseSet_ReaderFrameQueue_free)
static DTOR_WRAPPER_DEFINE(Vec_Fd_free)

/*!
    @see handshake_queue_drain (auth.c) — intentional duplicate
*/
static void writer_queue_drain(WriterFrameQueue* q) {
    const size_t count = WriterFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        free((void*)WriterFrameQueue_front(q)->ptr);
        WriterFrameQueue_pop_front(q);
    }
}

static void reader_queue_drain(ReaderFrameQueue* q) {
    const size_t count = ReaderFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const ReaderFrame* frame = ReaderFrameQueue_front(q);
        if (frame->status == READER_FRAME_OK) {
            free(frame->content.ptr);
        }
        ReaderFrameQueue_pop_front(q);
    }
}

int PlayerIo_init(PlayerIo* data, size_t max_entries) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (SparseSet_PlayerIoEntry_init(&data->entries, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_PlayerIoEntry_free, &data->entries);

    if (SparseSet_WriterFrameQueue_init(&data->write_qs, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_WriterFrameQueue_free, &data->write_qs);

    if (SparseSet_ReaderFrameQueue_init(&data->read_qs, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_ReaderFrameQueue_free, &data->read_qs);

    if (Vec_Fd_init(&data->players_reading, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Vec_Fd_free, &data->players_reading);

    if (Vec_Fd_init(&data->players_writing, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Vec_Fd_free, &data->players_writing);

    if (Vec_WriterQueueStatusEntry_init(&data->vec_write_qs_status, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

void PlayerIo_reset(PlayerIo* data) {
    for (size_t i = 0; i < SparseSet_ReaderFrameQueue_size(&data->read_qs); i++) {
        reader_queue_drain(SparseSet_ReaderFrameQueue_at_idx(&data->read_qs, i));
    }
    SparseSet_ReaderFrameQueue_reset(&data->read_qs);
    Vec_Fd_reset(&data->players_reading);
    Vec_Fd_reset(&data->players_writing);
    Vec_WriterQueueStatusEntry_reset(&data->vec_write_qs_status);
}

void PlayerIo_free(PlayerIo* data) {
    for (size_t i = 0; i < SparseSet_PlayerIoEntry_size(&data->entries); i++) {
        const size_t fd = SparseSet_PlayerIoEntry_key_at_idx(&data->entries, i);
        PlayerIoEntry* entry = SparseSet_PlayerIoEntry_at_idx(&data->entries, i);
        reader_free(&entry->reader);
        writer_free(&entry->writer);

        WriterFrameQueue* wq = SparseSet_WriterFrameQueue_value_at(&data->write_qs, fd);
        writer_queue_drain(wq);
        WriterFrameQueue_free(wq);

        ReaderFrameQueue* rq = SparseSet_ReaderFrameQueue_value_at(&data->read_qs, fd);
        reader_queue_drain(rq);
        ReaderFrameQueue_free(rq);
    }
    Vec_WriterQueueStatusEntry_free(&data->vec_write_qs_status);
    Vec_Fd_free(&data->players_writing);
    Vec_Fd_free(&data->players_reading);
    SparseSet_ReaderFrameQueue_free(&data->read_qs);
    SparseSet_WriterFrameQueue_free(&data->write_qs);
    SparseSet_PlayerIoEntry_free(&data->entries);
}

void PlayerIo_accept(PlayerIo* data, const Vec_Fd* fds, SparseSet_bool* err_fds, size_t queue_capacity) {
    for (size_t i = 0; i < Vec_Fd_size(fds); i++) {
        const Fd fd_raw = *Vec_Fd_at(fds, i);
        assert(fd_raw >= 0 && (size_t)fd_raw < data->entries.capacity);
        const size_t fd = (size_t)fd_raw;

        if (SparseSet_PlayerIoEntry_contains(&data->entries, fd)) {
            assert(false && "accepted fd must not already be in entries");
            continue;
        }
        if (SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }

        WriterFrameQueue* wq = SparseSet_WriterFrameQueue_value_at(&data->write_qs, fd);
        if (WriterFrameQueue_init(wq, queue_capacity) == -1) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            continue;
        }
        ReaderFrameQueue* rq = SparseSet_ReaderFrameQueue_value_at(&data->read_qs, fd);
        if (ReaderFrameQueue_init(rq, queue_capacity) == -1) {
            WriterFrameQueue_free(wq);
            *SparseSet_bool_activate(err_fds, fd) = true;
            continue;
        }

        PlayerIoEntry entry;
        reader_init(&entry.reader);
        writer_init(&entry.writer);
        const int err = SparseSet_PlayerIoEntry_insert(&data->entries, fd, &entry);
        assert(err != -1);
        (void)err;
    }
}

void PlayerIo_close(PlayerIo* data, const SparseSet_bool* close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(close_fds); i++) {
        const size_t fd = SparseSet_bool_key_at_idx(close_fds, i);
        if (!SparseSet_PlayerIoEntry_contains(&data->entries, fd)) {
            continue;
        }
        PlayerIoEntry* entry = SparseSet_PlayerIoEntry_get(&data->entries, fd);
        reader_free(&entry->reader);
        writer_free(&entry->writer);

        WriterFrameQueue* wq = SparseSet_WriterFrameQueue_value_at(&data->write_qs, fd);
        writer_queue_drain(wq);
        WriterFrameQueue_free(wq);
        if (SparseSet_WriterFrameQueue_contains(&data->write_qs, fd)) {
            SparseSet_WriterFrameQueue_erase(&data->write_qs, fd);
        }

        ReaderFrameQueue* rq = SparseSet_ReaderFrameQueue_value_at(&data->read_qs, fd);
        reader_queue_drain(rq);
        ReaderFrameQueue_free(rq);
        if (SparseSet_ReaderFrameQueue_contains(&data->read_qs, fd)) {
            SparseSet_ReaderFrameQueue_erase(&data->read_qs, fd);
        }

        SparseSet_PlayerIoEntry_erase(&data->entries, fd);
    }
}

void PlayerIo_read(PlayerIo* data, const Vec_Fd* m_players_reading, SparseSet_ReaderFrameQueue* m_read_qs, SparseSet_bool* err_fds) {
    for (size_t i = 0; i < Vec_Fd_size(m_players_reading); i++) {
        const size_t fd = (size_t)*Vec_Fd_at(m_players_reading, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "players_reading fd must not be in err_fds");
            continue;
        }
        ReaderFrameQueue* q = SparseSet_ReaderFrameQueue_activate(m_read_qs, fd);
        PlayerIoEntry* entry = SparseSet_PlayerIoEntry_get(&data->entries, fd);
        if (reader_recv(&entry->reader, (int)fd, q) == -1) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            // frames already queued stay for the close fan-out to reclaim
            SparseSet_ReaderFrameQueue_erase(m_read_qs, fd);
        }
        else if (ReaderFrameQueue_empty(q)) {
            SparseSet_ReaderFrameQueue_erase(m_read_qs, fd);
        }
    }
}

/*
    Returns -1 iff the fd failed and its slot was drained and erased from
    m_write_qs (so a dense-index iteration must not advance).
*/
static int write_one(PlayerIo* data, SparseSet_WriterFrameQueue* m_write_qs, SparseSet_bool* err_fds, size_t fd) {
    WriterFrameQueue* q = SparseSet_WriterFrameQueue_get(m_write_qs, fd);
    PlayerIoEntry* entry = SparseSet_PlayerIoEntry_get(&data->entries, fd);
    if (writer_send(&entry->writer, (int)fd, q) == -1) {
        *SparseSet_bool_activate(err_fds, fd) = true;
        writer_queue_drain(q);
        SparseSet_WriterFrameQueue_erase(m_write_qs, fd);
        return -1;
    }
    return 0;
}

void PlayerIo_write(PlayerIo* data, SparseSet_WriterFrameQueue* m_write_qs, const Vec_Fd* m_players_writing, SparseSet_bool* err_fds, Vec_WriterQueueStatusEntry* m_vec_write_qs_status) {
    /*
        Every fd with a nonempty queue has an active slot (leftovers keep their
        slot across ticks), so one pass over the dense list visits every
        drain candidate; fds already in err_fds are left for close to reclaim.
    */
    size_t i = 0;
    while (i < SparseSet_WriterFrameQueue_size(m_write_qs)) {
        const size_t fd = SparseSet_WriterFrameQueue_key_at_idx(m_write_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            i++;
            continue;
        }
        if (write_one(data, m_write_qs, err_fds, fd) == 0) {
            i++;
        }
    }

#ifndef NDEBUG
    for (size_t j = 0; j < Vec_Fd_size(m_players_writing); j++) {
        const size_t fd = (size_t)*Vec_Fd_at(m_players_writing, j);
        if (SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }
        if (SparseSet_WriterFrameQueue_contains(m_write_qs, fd)) {
            continue;
        }
        assert(false && "players_writing implies an active nonempty slot");
    }
#else
    (void)m_players_writing;
#endif

    i = 0;
    while (i < SparseSet_WriterFrameQueue_size(m_write_qs)) {
        const size_t fd = SparseSet_WriterFrameQueue_key_at_idx(m_write_qs, i);
        WriterFrameQueue* q = SparseSet_WriterFrameQueue_at_idx(m_write_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            i++;
            continue;
        }
        const size_t len = WriterFrameQueue_size(q);
        WriterQueueStatusEntry status;
        status.fd = (Fd)fd;

        PlayerIoEntry* entry = SparseSet_PlayerIoEntry_get(&data->entries, fd);

        const bool is_empty = len == 0 && entry->writer.state == WRITER_IDLE;
        if (is_empty) {
            status.status = WRITER_QUEUE_EMPTY;
        }
        else if (len == WriterFrameQueue_capacity(q)) {
            status.status = WRITER_QUEUE_FULL;
        }
        else {
            status.status = WRITER_QUEUE_NORMAL;
        }
        const int err = Vec_WriterQueueStatusEntry_push_back(m_vec_write_qs_status, &status);
        assert(err != -1);
        (void)err;
        if (is_empty) {
            SparseSet_WriterFrameQueue_erase(m_write_qs, fd);
        }
        else {
            i++;
        }
    }
}
