#include "auth.h"
#include "wire.h"
#include <stdlib.h>
#include <string.h>

static void auth_queue_drain(AuthFrameQueue* q) {
    const size_t count = AuthFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const AuthFrame* const frame = AuthFrameQueue_front(q);
        if (frame->status == FRAME_OK) {
            free(frame->frame.ptr);
        }
        AuthFrameQueue_pop_front(q);
    }
}

static void writer_queue_drain(WriterFrameQueue* q) {
    const size_t count = WriterFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        free((void*)WriterFrameQueue_front(q)->ptr);
        WriterFrameQueue_pop_front(q);
    }
}

int AuthData_init(AuthData* data, const unsigned char key[SESSION_KEY_LEN], size_t queue_capacity) {
    if (AuthFrameQueue_init(&data->decrypt_q, queue_capacity) == -1) {
        return -1;
    }
    if (WriterFrameQueue_init(&data->encrypt_q, queue_capacity) == -1) {
        AuthFrameQueue_free(&data->decrypt_q);
        return -1;
    }
    memcpy(data->key, key, sizeof(data->key));
    return 0;
}

void AuthData_free(AuthData* data) {
    auth_queue_drain(&data->decrypt_q);
    AuthFrameQueue_free(&data->decrypt_q);
    writer_queue_drain(&data->encrypt_q);
    WriterFrameQueue_free(&data->encrypt_q);
    memset(data->key, 0, sizeof(data->key));
}

void AuthData_reset(AuthData* data) {
    auth_queue_drain(&data->decrypt_q);
}

void AuthData_decrypt(AuthData* data, const ReaderFrameQueue* m_read_q,
                      AuthFrameQueue* m_decrypt_q, ClientFault* fault) {
    if (*fault != FAULT_NONE) {
        return;
    }

    const size_t count = ReaderFrameQueue_size(m_read_q);
    for (size_t i = 0; i < count; i++) {
        const ReaderFrame* const frame = ReaderFrameQueue_at(m_read_q, i);

        AuthFrame decrypted;
        memset(&decrypted, 0, sizeof(decrypted));

        if (frame->status != READER_FRAME_OK) {
            decrypted.status = FRAME_PAYLOAD_TOO_LARGE;
        }
        else {
            uint32_t plain_len;
            unsigned char* const plain = tetrish_session_decrypt(
                &data->key, frame->content.ptr, (uint32_t)frame->content.length, &plain_len);
            if (plain == NULL) {
                decrypted.status = FRAME_DECRYPT_ERROR;
            }
            else {
                decrypted.frame.ptr = plain;
                decrypted.frame.length = plain_len;
                decrypted.status = FRAME_OK;
            }
        }

        if (AuthFrameQueue_push_back(m_decrypt_q, &decrypted) == -1) {
            // read_q and decrypt_q share a capacity, so this cannot happen
            // within the contract; treat it as the local failure it is
            free(decrypted.frame.ptr);
            *fault = FAULT_LOCAL;
            return;
        }
    }
}

void AuthData_encrypt(AuthData* data, WriterFrameQueue* m_encrypt_q,
                      WriterFrameQueue* m_write_q, ClientFault* fault) {
    if (*fault != FAULT_NONE) {
        writer_queue_drain(m_encrypt_q);
        return;
    }

    while (!WriterFrameQueue_empty(m_encrypt_q)) {
        const WriterFrame plain = *WriterFrameQueue_front(m_encrypt_q);

        uint32_t cipher_len;
        unsigned char* const cipher = tetrish_session_encrypt(
            &data->key, plain.ptr, (uint32_t)plain.length, &cipher_len);

        if (cipher == NULL || cipher_len == 0 || cipher_len > FRAME_MAX) {
            free(cipher);
            free((void*)plain.ptr);
            WriterFrameQueue_pop_front(m_encrypt_q);
            *fault = FAULT_TRANSPORT;
            writer_queue_drain(m_encrypt_q);
            return;
        }

        const WriterFrame cipher_frame = {cipher, cipher_len};
        if (WriterFrameQueue_push_back(m_write_q, &cipher_frame) == -1) {
            // the socket has not kept up: leave the rest staged and retry next
            // tick, which is when POLLOUT will have drained room for it
            free(cipher);
            return;
        }

        free((void*)plain.ptr);
        WriterFrameQueue_pop_front(m_encrypt_q);
    }
}
