#include "state.h"
#include "dtor.h"
#include "htttp.h"
#include "tetrissh.h"
#include <assert.h>
#include <sys/epoll.h>

static DTOR_WRAPPER_DEFINE(htttp_message_free)
static DTOR_WRAPPER_DEFINE(free)

static int print_client_message(SessionKey* key, int fd, struct frame* frame) {
    uint32_t out_len;
    unsigned char* buf = tetrish_session_decrypt(key, frame->buf, frame->len, &out_len);

    if (buf == NULL) {
        fprintf(stderr, "cannot print message from client %d\n", fd);
        return -1;
    }

    printf("client %d sent: %.*s\n", fd, (int)out_len, buf);
    fflush(stdout);
    free(buf);
    return 0;
}

int transit_read(struct client* c, TetrishCredential* credential) {
    switch (c->auth_state) {
        case CLIENT_AUTH_NONCE: {
            assert(c->frame_count == 1);

            struct frame frames[2] = {0};
            struct frame* frame_nonce = &frames[0];

            frame_nonce->is_heap_allocated = true;
            struct frame* f = &c->frame[0];
            if ((frame_nonce->buf = tetrish_server_sign_nonce(f->buf, f->len, credential->private_key, &frame_nonce->len)) == NULL) {
                return -1;
            }
            client_pop_frame(c, 1);
            encode_u32_be(frame_nonce->len_buf, frame_nonce->len);

            struct frame frame_auth = {0};
            frame_auth.is_heap_allocated = false;
            frame_auth.buf = credential->certificate;
            frame_auth.len = credential->certificate_len;
            encode_u32_be(frame_auth.len_buf, frame_auth.len);

            frames[1] = frame_auth;

            client_push_frame(c, frames, sizeof(frames)/sizeof(struct frame));
            client_transit_state(c, CLIENT_AUTH_SYMKEY, CLIENT_WRITING, 2);
            return 0;
        }
        case CLIENT_AUTH_SYMKEY: {
            assert(c->frame_count == 1);

            struct frame* f = &c->frame[0];
            unsigned char* buf;
            uint32_t len;
            if ((buf = tetrish_server_decrypt_session_key(f->buf, f->len, credential, &len)) == NULL) {
                return -1;
            }
            client_pop_frame(c, 1);
            assert(len == SESSION_KEY_LEN);
            memcpy(c->session_key, buf, SESSION_KEY_LEN);
            free(buf);

            client_transit_state(c, CLIENT_AUTH_SUCCESS, CLIENT_READING_LEN, 1);
            return 0;
        }
        case CLIENT_AUTH_SUCCESS: {
            assert(c->frame_count == 1);

            if (print_client_message(&c->session_key, c->fd, &c->frame[0]) == -1) {
                return -1;
            }

            // for now, assume the response message does not use the frame.
            client_pop_frame(c, 1);
            DTOR_DEFINE(dtor, 10);

            htttp_message_t message;
            if (htttp_make_response(&message, 200, "OK", "Accepted", "text") == -1) {
                DTOR_RETURN(dtor, -1);
            }
            DTOR_INSERT(dtor, htttp_message_free, &message);

            unsigned char* buffer;
            size_t length;
            if (htttp_serialize(&message, &buffer, &length) == -1) {
                perror("serialize");
                DTOR_RETURN(dtor, -1);
            }
            DTOR_INSERT(dtor, free, buffer);

            size_t out_len_sz;
            unsigned char* out_buffer = session_encrypt(c->session_key, buffer, length, &out_len_sz);

            if (out_buffer == NULL) {
                DTOR_RETURN(dtor, -1);
            }

            if (out_len_sz > FRAME_MAX) {
                fprintf(stderr, "Message too long");
                free(out_buffer);
                DTOR_RETURN(dtor, -1);
            }

            struct frame frame = {0};
            frame.is_heap_allocated = true;
            frame.buf = out_buffer;
            frame.len = (uint32_t)out_len_sz;
            encode_u32_be(frame.len_buf, frame.len);
            client_push_frame(c, &frame, 1);
            client_transit_state(c, CLIENT_AUTH_SUCCESS, CLIENT_WRITING, 1);

            DTOR_RETURN(dtor, 0);
        }
    }

    return -1;
}

