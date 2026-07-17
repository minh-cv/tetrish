#include "tetris_client.h"
#include "client_io.h"
#include "dtor.h"
#include "htttp.h"
#include "tetrissh.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

static DTOR_WRAPPER_DEFINE(htttp_message_free)
static DTOR_WRAPPER_DEFINE(free)

static TetrisClient* upcast(struct ClientIo* c_base) {
    size_t diff = offsetof(TetrisClient, base);
    return (TetrisClient*)((char*)c_base - diff);
}

static int print_client_message(TetrisClient* c) {
    struct ClientIoFrame* f = client_io_get_top_frame(&c->base);
    uint32_t out_len;
    unsigned char* buf = tetrish_session_decrypt(&c->session_key, f->buf, f->len, &out_len);

    if (buf == NULL) {
        fprintf(stderr, "cannot print message from client %d\n", c->base.fd);
        return -1;
    }

    printf("client %d sent: %.*s\n", c->base.fd, (int)out_len, buf);
    fflush(stdout);
    free(buf);
    return 0;
}

// game logic is not implemented yet - this just echoes a fixed 200 OK, same as the
// pre-refactor tetrisd/state.c did.
ClientIoResult tetris_client_transist_read(int epoll_fd, struct ClientIo* c_base) {
    TetrisClient* c = upcast(c_base);

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
        perror("serialize");
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }
    DTOR_INSERT(dtor, free, buffer);

    uint32_t out_len;
    unsigned char* out_buffer = tetrish_session_encrypt(&c->session_key, buffer, (uint32_t)length, &out_len);
    if (out_buffer == NULL) {
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }

    if (out_len > FRAME_MAX) {
        fprintf(stderr, "message too long\n");
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

    if (mod_epoll_events(epoll_fd, c_base->fd, EPOLLOUT | EPOLLRDHUP) == -1) {
        DTOR_RETURN(dtor, CLIENT_IO_ERR);
    }

    DTOR_RETURN(dtor, CLIENT_IO_OK);
}

ClientIoResult tetris_client_transist_write(int epoll_fd, struct ClientIo* c_base) {
    client_io_transit_state(c_base, CLIENT_READING_LEN, 1);

    if (mod_epoll_events(epoll_fd, c_base->fd, EPOLLIN | EPOLLRDHUP) == -1) {
        return CLIENT_IO_ERR;
    }

    return CLIENT_IO_OK;
}

void tetris_client_init(TetrisClient* c, int client_fd, SessionKey* key) {
    client_io_init(&c->base, client_fd);
    memcpy(c->session_key, key, sizeof(c->session_key));
    client_io_transit_state(&c->base, CLIENT_READING_LEN, 1);
}

void tetris_client_free(TetrisClient* c) {
    client_io_free(&c->base);
}
