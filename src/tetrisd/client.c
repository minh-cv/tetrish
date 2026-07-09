#include "client.h"

#include <assert.h>

void frame_free(struct frame* frame) {
    if (frame->is_heap_allocated) {
        free(frame->buf);
    }
}

void client_pop_frame(struct client* c, unsigned int count) {
    assert(count <= c->frame_count);
    for (; count > 0; count--) {
        c->frame_count--;
        frame_free(&c->frame[c->frame_count]);
    }
}

void client_push_frame(struct client* c, const struct frame frames[], unsigned int count) {
    assert(count <= 5 - c->frame_count);
    for (unsigned int i = 0; i < count; i++) {
        c->frame[c->frame_count] = frames[count - i - 1];
        c->frame_count++;
    }
}

void client_transit_state(struct client* c, enum client_auth_state auth_state, enum client_state write_state, unsigned int frame_active) {
    assert(frame_active <= 5 - c->frame_count);
    c->auth_state = auth_state;
    c->state = write_state;
    c->frame_active = frame_active;

    if (write_state == CLIENT_READING_LEN) {
        memset(c->frame + c->frame_count, 0, sizeof(struct frame)*frame_active);
    }
}
