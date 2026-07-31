#ifndef TETRISH_TETRISD_PLAYER_H
#define TETRISH_TETRISD_PLAYER_H

#include "type.h"
#include <stdbool.h>

/*!
    @brief Turn buffered bytes into protocol progress: handshake steps, parsed
    requests, and replies queued back for the writer.

    @note does no socket I/O of its own; the caller moves the bytes and calls
    this once the reader/writer have run.
    @return 0 to keep the connection, -1 to have the caller close it.
*/
int player_process(Server* server, PlayerFdData* player);

/*!
    @return whether the writer still holds a partial frame or the queue is
    non-empty, i.e. whether EPOLLOUT is worth arming.
*/
bool player_wants_write(const PlayerFdData* player);

/*!
    @brief Release everything the slot owns and mark it free.

    @note neither `reader_free` nor `writer_free` touches the payloads of frames
    that already completed, so draining both queues is this function's problem.
    `reader_free` does release the reader's own ring buffer; the writer's is
    caller-owned, so it needs an explicit `WriterFrameQueue_free`.
*/
void player_slot_free(PlayerFdData* slot);

#endif
