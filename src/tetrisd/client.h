#ifndef TETRISH_TETRISD_CLIENT_H
#define TETRISH_TETRISD_CLIENT_H

#include "common.h"
#include <stdbool.h>

enum client_state {
    CLIENT_READING_LEN,
    CLIENT_READING_BODY,
    CLIENT_WRITING,
};

enum client_auth_state {
    CLIENT_AUTH_NONCE,
    CLIENT_AUTH_SYMKEY,
    CLIENT_AUTH_SUCCESS,
};

typedef struct frame {
    uint8_t len_buf[4];
    uint32_t len_used;

    unsigned char* buf;
    uint32_t len;
    uint32_t used;

    bool is_heap_allocated;
} frame;

struct client {
    int fd;
    enum client_state state;
    enum client_auth_state auth_state;

    //! @brief a stack-like object containing frame, tracked by `frame_count`
    frame frame[5];

    //! @brief the number of frame objects currently available
    unsigned int frame_count;

    //! @brief the number of frame objects left to read/write from
    unsigned int frame_active;

    unsigned char session_key[SESSION_KEY_LEN];
};

void frame_free(struct frame* frame);
void client_pop_frame(struct client* c, unsigned int count);
void client_push_frame(struct client* c, const struct frame frames[], unsigned int count);
void client_transit_state(struct client* c, enum client_auth_state auth_state, enum client_state write_state, unsigned int frame_active);


#endif