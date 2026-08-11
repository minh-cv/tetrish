#ifndef TETRISH_TETRISU_SERVER_IO_H
#define TETRISH_TETRISU_SERVER_IO_H

#include "type.h"

/*!
    @brief tetrisd's PlayerIo with the sparse sets removed.

    It owns the corestack Reader/Writer pair and the two frame queues either
    side of them, and it is the only layer that touches the socket. It does not
    own the descriptor: Connector does.
*/
typedef struct {
    Reader reader;
    Writer writer;
    ReaderFrameQueue read_q;
    WriterFrameQueue write_q;
    bool write_pending;    // write_q nonempty or a partial frame is in flight
} ServerIo;

int ServerIo_init(ServerIo* data, size_t queue_capacity);
void ServerIo_free(ServerIo* data);

/*!
    @post all frames in @c read_q are freed; @c write_q is untouched, since
          pending writes survive ticks until the socket is writable
*/
void ServerIo_reset(ServerIo* data);

/*!
    @brief one read pass, appending complete frames to @p m_read_q

    @post a socket error or a clean EOF sets @p fault to FAULT_TRANSPORT
    @post an oversized frame is in-band: it arrives with a non-OK
          ReaderFrameStatus rather than faulting the connection
*/
void ServerIo_read(ServerIo* data, int fd, ReaderFrameQueue* m_read_q, ClientFault* fault);

/*!
    @brief drain @p m_write_q to the socket as far as it accepts

    @post fully flushed frames are popped and freed; a socket error sets
          @p fault to FAULT_TRANSPORT
    @post @c write_pending reflects the queue level for Poller_sync_interest
*/
void ServerIo_write(ServerIo* data, int fd, WriterFrameQueue* m_write_q, ClientFault* fault);

#endif
