#include "server_io.h"
#include <stdlib.h>

/*!
    @see reader_queue_drain (src/tetrisd/player_io.c) — intentional duplicate
*/
static void reader_queue_drain(ReaderFrameQueue* q) {
    const size_t count = ReaderFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const ReaderFrame* const frame = ReaderFrameQueue_front(q);
        if (frame->status == READER_FRAME_OK) {
            free(frame->content.ptr);
        }
        ReaderFrameQueue_pop_front(q);
    }
}

static void writer_queue_drain(WriterFrameQueue* q) {
    const size_t count = WriterFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        free((void*)WriterFrameQueue_front(q)->ptr);
        WriterFrameQueue_pop_front(q);
    }
}

int ServerIo_init(ServerIo* data, size_t queue_capacity) {
    if (ReaderFrameQueue_init(&data->read_q, queue_capacity) == -1) {
        return -1;
    }
    if (WriterFrameQueue_init(&data->write_q, queue_capacity) == -1) {
        ReaderFrameQueue_free(&data->read_q);
        return -1;
    }
    reader_init(&data->reader);
    writer_init(&data->writer);
    data->write_pending = false;
    return 0;
}

void ServerIo_free(ServerIo* data) {
    reader_free(&data->reader);
    writer_free(&data->writer);
    reader_queue_drain(&data->read_q);
    ReaderFrameQueue_free(&data->read_q);
    writer_queue_drain(&data->write_q);
    WriterFrameQueue_free(&data->write_q);
}

void ServerIo_reset(ServerIo* data) {
    reader_queue_drain(&data->read_q);
}

void ServerIo_read(ServerIo* data, int fd, ReaderFrameQueue* m_read_q, ClientFault* fault) {
    if (*fault != FAULT_NONE) {
        return;
    }
    if (reader_recv(&data->reader, fd, m_read_q) == -1) {
        // reader_recv reports a clean EOF the same way as a socket error, and
        // for a client both mean the same thing: the session is over
        *fault = FAULT_TRANSPORT;
    }
}

void ServerIo_write(ServerIo* data, int fd, WriterFrameQueue* m_write_q, ClientFault* fault) {
    // an idle writer with an empty queue makes this a no-op, so the call needs
    // no guard beyond the fault: a frame half-written in an earlier tick is
    // drained here too, even though the queue behind it is empty
    if (*fault == FAULT_NONE && writer_send(&data->writer, fd, m_write_q) == -1) {
        *fault = FAULT_TRANSPORT;
    }

    data->write_pending =
        !WriterFrameQueue_empty(m_write_q) || data->writer.state != WRITER_IDLE;
}
