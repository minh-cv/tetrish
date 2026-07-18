#include "tetris_client.h"
#include "client_io.h"
#include "dtor.h"
#include "htttp.h"
#include "logger.h"
#include "tetrissh.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(htttp_message_free)
static DTOR_WRAPPER_DEFINE(free)

static TetrisClient* downcast(struct ClientIo* c_base) {
    size_t diff = offsetof(TetrisClient, base);
    return (TetrisClient*)((char*)c_base - diff);
}

static int print_client_message(TetrisClient* c) {
    struct ClientIoFrame* f = client_io_get_top_frame(&c->base);
    uint32_t out_len;
    unsigned char* buf = tetrish_session_decrypt(&c->session_key, f->buf, f->len, &out_len);

    if (buf == NULL) {
        LOGGER_LOG(LOG_ERROR, "client", "cannot print message from client %d", c->base.fd);
        return -1;
    }

    LOGGER_LOG(LOG_INFO, "client", "client %d sent: %.*s", c->base.fd, (int)out_len, buf);
    free(buf);
    return 0;
}

// game logic is not implemented yet - this just echoes a fixed 200 OK, same as the
// pre-refactor tetrisd/state.c did.
ClientIoResult tetris_client_transist_read(struct ClientIo* c_base) {
    TetrisClient* c = downcast(c_base);

    if (c->state == TETRIS_CLIENT_START) {
        c->state = TETRIS_CLIENT_ACTIVE;
        client_io_transit_state(c_base, CLIENT_READING_LEN, 1);
        return CLIENT_IO_CONTINUE;
    }

    if (print_client_message(c) == -1) {
        return CLIENT_IO_ERR;
    }
    client_io_pop_frame(c_base, 1);

    DTOR_DEFINE(dtor, 10);

    htttp_message_t message;
    if (htttp_make_response(&message, 200, "OK", "Accepted", "text") == -1) {
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }
    DTOR_INSERT(dtor, htttp_message_free, &message);

    unsigned char* buffer;
    size_t length;
    if (htttp_serialize(&message, &buffer, &length) == -1) {
        LOGGER_PERROR("client", "serialize");
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }
    DTOR_INSERT(dtor, free, buffer);

    uint32_t out_len;
    unsigned char* out_buffer = tetrish_session_encrypt(&c->session_key, buffer, (uint32_t)length, &out_len);
    if (out_buffer == NULL) {
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }

    if (out_len > FRAME_MAX) {
        LOGGER_LOG(LOG_ERROR, "client", "message too long");
        free(out_buffer);
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }

    struct ClientIoFrame frame = {0};
    frame.is_heap_allocated = true;
    frame.buf = out_buffer;
    frame.len = out_len;
    encode_u32_be(frame.len_buf, frame.len);
    client_io_push_frame(c_base, &frame, 1);
    client_io_transit_state(c_base, CLIENT_WRITING, 1);

    DTOR_RETURN(dtor, CLIENT_IO_CONTINUE);
}

ClientIoResult tetris_client_transist_write(struct ClientIo* c_base) {
    client_io_transit_state(c_base, CLIENT_READING_LEN, 1);

    return CLIENT_IO_CONTINUE;
}

void tetris_client_init(TetrisClient* c, int client_fd, SessionKey* key) {
    client_io_init(&c->base, client_fd);
    memcpy(c->session_key, key, sizeof(c->session_key));
    client_io_transit_state(&c->base, CLIENT_READ_TRANSIT, 0);
    c->state = TETRIS_CLIENT_START;
}

void tetris_client_free(TetrisClient* c) {
    client_io_free(&c->base);
}
