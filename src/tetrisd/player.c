#include "player.h"
#include "htttp.h"
#include "logger.h"
#include "network/reader.h"
#include "network/writer.h"
#include "tetrissh.h"
#include "type.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool player_wants_write(const PlayerFdData* player) {
    return player->writer.state != WRITER_IDLE || !WriterFrameQueue_empty(&player->write_queue);
}

void player_slot_free(PlayerFdData* slot) {
    while (!ReaderFrameQueue_empty(&slot->reader.queue)) {
        free(ReaderFrameQueue_front(&slot->reader.queue)->ptr);
        ReaderFrameQueue_pop_front(&slot->reader.queue);
    }
    reader_free(&slot->reader);

    while (!WriterFrameQueue_empty(&slot->write_queue)) {
        free((void*)WriterFrameQueue_front(&slot->write_queue)->ptr);
        WriterFrameQueue_pop_front(&slot->write_queue);
    }
    WriterFrameQueue_free(&slot->write_queue);
    writer_free(&slot->writer);

    slot->auth_state = PLAYER_AUTH_NONCE;
    slot->fd = -1;
}

/*!
    @note takes ownership of `buf`, including on failure: the writer frees every
    frame it dequeues, so the queue is the only owner once the push succeeds.
*/
static int player_queue_frame(PlayerFdData* player, const unsigned char* buf, size_t length) {
    if (length == 0 || length > FRAME_MAX) {
        LOGGER_LOG(LOG_ERROR, "client", "fd=%d frame of length %zu discarded", player->fd, length);
        free((void*)buf);
        return -1;
    }

    const WriterFrame frame = {
        buf,
        length,
    };

    if (WriterFrameQueue_push_back(&player->write_queue, &frame) == -1) {
        LOGGER_LOG(LOG_WARN, "client", "fd=%d write queue full", player->fd);
        free((void*)buf);
        return -1;
    }

    return 0;
}

static int player_handle_nonce(PlayerFdData* player, const ReaderFrame* frame, TetrishCredential* credential) {
    if (frame->length != NONCE_LEN) {
        LOGGER_LOG(LOG_WARN, "auth", "fd=%d nonce of length %zu rejected", player->fd, frame->length);
        return -1;
    }

    uint32_t signed_nonce_len;
    unsigned char* signed_nonce = tetrish_server_sign_nonce(
        frame->ptr, 
        NONCE_LEN,    
        credential->private_key, 
        &signed_nonce_len);
    if (signed_nonce == NULL) {
        LOGGER_LOG(LOG_ERROR, "auth", "fd=%d cannot sign nonce", player->fd);
        return -1;
    }

    // the writer frees what it sends, so the certificate goes out as a copy.
    unsigned char* certificate = malloc(credential->certificate_len);
    if (certificate == NULL) {
        free(signed_nonce);
        return -1;
    }
    memcpy(certificate, credential->certificate, credential->certificate_len);

    // order is load-bearing: the client reads the signature, then the certificate.
    int val = player_queue_frame(player, signed_nonce, signed_nonce_len);
    if (val == -1) {
        assert(false);
        free(certificate);
        return -1;
    }

    val = player_queue_frame(player, certificate, credential->certificate_len);
    if (val == -1) {
        assert(false);
        return -1;
    }

    player->auth_state = PLAYER_AUTH_SYMKEY;

    return 0;
}

/*!
    @note overwrites the `unauthed` arm of the union, so every read of it must
    happen before this call.
*/
static void player_promote(PlayerFdData* player, const unsigned char* key) {
    player->auth_state = PLAYER_AUTH_DONE;
    player->room = NULL;
    player->name[0] = '\0';
    memcpy(player->key, key, SESSION_KEY_LEN);
}

static int player_handle_session_key(PlayerFdData* player, const ReaderFrame* frame, TetrishCredential* credential) {
    if (frame->length > UINT32_MAX) {
        return -1;
    }

    uint32_t key_len;
    unsigned char* key = tetrish_server_decrypt_session_key(
        frame->ptr, 
        (uint32_t)frame->length,
        credential, 
        &key_len);
    if (key == NULL) {
        LOGGER_LOG(LOG_WARN, "auth", "fd=%d cannot decrypt session key", player->fd);
        return -1;
    }
    assert(key_len == SESSION_KEY_LEN);

    player_promote(player, key);
    free(key);

    LOGGER_LOG(LOG_INFO, "auth", "fd=%d authenticated", player->fd);
    return 0;
}

static int htttp_message_from_frame(HtttpMessage* message, unsigned char** message_buf, const ReaderFrame* frame, SessionKey* key) {
    if (frame->length > UINT32_MAX) {
        return -1;
    }

    uint32_t plaintext_len;
    unsigned char* plaintext = tetrish_session_decrypt(
        key, 
        frame->ptr,
        (uint32_t)frame->length, 
        &plaintext_len);
    if (plaintext == NULL) {
        return -1;
    }

    int val = htttp_parse(plaintext, plaintext_len, message);
    if (val == -1) {
        free(plaintext);
        return -1;
    }
    *message_buf = plaintext;

    return 0;
}

static int frame_from_htttp_message(WriterFrame* frame, const HtttpMessage* response, SessionKey* key) {
    size_t plaintext_len;
    unsigned char* plaintext = htttp_serialize(response, &plaintext_len);

    if (plaintext == NULL) {
        return -1;
    }

    if (plaintext_len > UINT32_MAX) {
        free(plaintext);
        return -1;
    }

    uint32_t ciphertext_len;
    unsigned char* ciphertext = tetrish_session_encrypt(
        key, 
        plaintext,                                                
        (uint32_t)plaintext_len, 
        &ciphertext_len);
    free(plaintext);
    
    if (ciphertext == NULL) {
        return -1;
    }

    frame->length = ciphertext_len;
    frame->ptr = ciphertext;

    return 0;
}

typedef struct {
    bool is_method_owned;
    bool is_path_owned;
    bool is_reason_owned;
    bool is_key_owned[HTTTP_HEADER_MAX];
    bool is_value_owned[HTTTP_HEADER_MAX];
    bool is_body_owned;
} HtttpMessageFieldOwnership;

static void htttp_message_free(HtttpMessage* message, const HtttpMessageFieldOwnership* ownership) {
    if (message->is_request) {
        if (ownership->is_method_owned) 
            free((void*)message->request.method);
        if (ownership->is_path_owned) 
            free((void*)message->request.path);
        for (size_t i = 0; i < message->request.header_count; i++) {
            if (ownership->is_key_owned[i])
                free((void *)message->request.header[i].key);
            if (ownership->is_value_owned[i])
                free((void *)message->request.header[i].value);
        }
        if (ownership->is_body_owned)
            free((void *)message->request.body);
    }
    else {
        if (ownership->is_reason_owned)
            free((void *)message->response.reason);
        for (size_t i = 0; i < message->response.header_count; i++) {
            if (ownership->is_key_owned[i])
                free((void *)message->response.header[i].key);
            if (ownership->is_value_owned[i])
                free((void *)message->response.header[i].value);
        }
        if (ownership->is_body_owned)
            free((void *)message->response.body);
    }
}

static int player_handle_request(PlayerFdData* player, HtttpMessage* request, HtttpMessage* response, HtttpMessageFieldOwnership* ownership) {
    (void)request; (void)player;
    HtttpMessage new_response = {
        {
            .response = {
                200,
                "OK",
                {
                    {
                        "Content-Length",
                        "0",
                    },
                    {
                        "Content-Type",
                        "text/plain",
                    },
                },
                2,
                NULL,
                0,
            }
        },
        false,
    };
    HtttpMessageFieldOwnership new_ownership = {
        false,
        false,
        false,
        {false, false,},
        {false, false,},
        false,
    };
    *response = new_response;
    *ownership = new_ownership;
    return 0;
}

/*!
    @return -1 if the client should be closed, 0 otherwise 
*/
static int player_handle_request_frame(PlayerFdData* player, const ReaderFrame* frame) {
    HtttpMessage request;
    unsigned char* request_buf;
    if (htttp_message_from_frame(&request, &request_buf, frame, &player->key) == -1) {
        return -1;
    }

    HtttpMessage response = {0};
    HtttpMessageFieldOwnership ownership = {0};
    int val = player_handle_request(player, &request, &response, &ownership);
    if (val == -1) {
        free(request_buf);
        return 0;
    }

    WriterFrame writer_frame;
    val = frame_from_htttp_message(&writer_frame, &response, &player->key);
    htttp_message_free(&response, &ownership);
    free(request_buf);
    if (val == -1) {
        return -1;
    }

    val = player_queue_frame(player, writer_frame.ptr, writer_frame.length);
    if (val == -1) {
        assert(false);
        return 0;
    }

    return 0;
}

static int player_process_single_frame(Server* server, PlayerFdData* player, ReaderFrame* frame) {
    switch (player->auth_state) {
        case PLAYER_AUTH_NONCE:
        return player_handle_nonce(player, frame, &server->credential);
        
        case PLAYER_AUTH_SYMKEY:
        return player_handle_session_key(player, frame, &server->credential);
        
        case PLAYER_AUTH_DONE:
        return player_handle_request_frame(player, frame);
        
        default:
        assert(false);
        return -1;
    }
}

int player_process(Server* server, PlayerFdData* player) {
    ReaderFrameQueue* queue = &player->reader.queue;
    int val = 0;

    while (
        val == 0 && 
        !ReaderFrameQueue_empty(queue) && 
        WriterFrameQueue_size(&player->write_queue) != WriterFrameQueue_capacity(&player->write_queue)
    ) {
        ReaderFrame frame = *ReaderFrameQueue_front(queue);
        ReaderFrameQueue_pop_front(queue);

        val = player_process_single_frame(server, player, &frame);

        free(frame.ptr);
    }

    return val;
}
