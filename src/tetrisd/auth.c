#include "auth.h"
#include "dtor.h"
#include "logger.h"
#include "network/reader.h"
#include "type.h"
#include "wire.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(tetrish_credential_free)
static DTOR_WRAPPER_DEFINE(SparseSet_AuthEntry_free)
static DTOR_WRAPPER_DEFINE(SparseSet_WriterFrameQueue_free)

static void auth_queue_drain(AuthFrameQueue* q) {
    const size_t count = AuthFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        const AuthFrame* frame = AuthFrameQueue_front(q);
        if (frame->status == FRAME_OK) {
            free(frame->frame.ptr);
        }
        AuthFrameQueue_pop_front(q);
    }
}

/*!
    @see writer_queue_drain (player_io.c) — intentional duplicate
*/
static void writer_queue_drain(WriterFrameQueue* q) {
    const size_t count = WriterFrameQueue_size(q);
    for (size_t i = 0; i < count; i++) {
        free((void*)WriterFrameQueue_front(q)->ptr);
        WriterFrameQueue_pop_front(q);
    }
}

int AuthData_init(AuthData* data, size_t max_entries, const char* key_path, const char* certificate_path) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (tetrish_credential_init(&data->credential, key_path, certificate_path) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, tetrish_credential_free, &data->credential);

    if (data->credential.certificate_len == 0 || data->credential.certificate_len > FRAME_MAX) {
        LOGGER_LOG(LOG_ERROR, "auth", "certificate does not fit in a frame");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    if (SparseSet_AuthEntry_init(&data->entries, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_AuthEntry_free, &data->entries);

    if (SparseSet_WriterFrameQueue_init(&data->encrypt_qs, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, SparseSet_WriterFrameQueue_free, &data->encrypt_qs);

    if (SparseSet_AuthFrameQueue_init(&data->decrypt_qs, max_entries) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

void AuthData_reset(AuthData* data) {
    for (size_t i = 0; i < SparseSet_WriterFrameQueue_size(&data->encrypt_qs); i++) {
        writer_queue_drain(SparseSet_WriterFrameQueue_at_idx(&data->encrypt_qs, i));
    }
    SparseSet_WriterFrameQueue_reset(&data->encrypt_qs);

    for (size_t i = 0; i < SparseSet_AuthFrameQueue_size(&data->decrypt_qs); i++) {
        auth_queue_drain(SparseSet_AuthFrameQueue_at_idx(&data->decrypt_qs, i));
    }
    SparseSet_AuthFrameQueue_reset(&data->decrypt_qs);
}

void AuthData_free(AuthData* data) {
    for (size_t i = 0; i < SparseSet_AuthEntry_size(&data->entries); i++) {
        const size_t fd = SparseSet_AuthEntry_key_at_idx(&data->entries, i);

        WriterFrameQueue* aq = SparseSet_WriterFrameQueue_value_at(&data->encrypt_qs, fd);
        writer_queue_drain(aq);
        WriterFrameQueue_free(aq);

        AuthFrameQueue* dq = SparseSet_AuthFrameQueue_value_at(&data->decrypt_qs, fd);
        auth_queue_drain(dq);
        AuthFrameQueue_free(dq);
    }
    SparseSet_AuthFrameQueue_free(&data->decrypt_qs);
    SparseSet_WriterFrameQueue_free(&data->encrypt_qs);
    SparseSet_AuthEntry_free(&data->entries);
    tetrish_credential_free(&data->credential);
}

void AuthData_accept(AuthData* data, const Vec_Fd* fds, SparseSet_bool* err_fds, size_t queue_capacity) {
    for (size_t i = 0; i < Vec_Fd_size(fds); i++) {
        const Fd fd_raw = *Vec_Fd_at(fds, i);
        assert(fd_raw >= 0 && (size_t)fd_raw < data->entries.capacity);
        const size_t fd = (size_t)fd_raw;

        if (SparseSet_AuthEntry_contains(&data->entries, fd)) {
            assert(false && "accepted fd must not already be in entries");
            continue;
        }
        if (SparseSet_bool_contains(err_fds, fd)) {
            continue;
        }

        WriterFrameQueue* aq = SparseSet_WriterFrameQueue_value_at(&data->encrypt_qs, fd);
        if (WriterFrameQueue_init(aq, queue_capacity) == -1) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            continue;
        }
        AuthFrameQueue* dq = SparseSet_AuthFrameQueue_value_at(&data->decrypt_qs, fd);
        if (AuthFrameQueue_init(dq, queue_capacity) == -1) {
            WriterFrameQueue_free(aq);
            *SparseSet_bool_activate(err_fds, fd) = true;
            continue;
        }

        AuthEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.auth_state = AUTH_NONCE;
        const int err = SparseSet_AuthEntry_insert(&data->entries, fd, &entry);
        assert(err != -1);
        (void)err;
    }
}

void AuthData_close(AuthData* data, const SparseSet_bool* close_fds) {
    for (size_t i = 0; i < SparseSet_bool_size(close_fds); i++) {
        const size_t fd = SparseSet_bool_key_at_idx(close_fds, i);
        if (!SparseSet_AuthEntry_contains(&data->entries, fd)) {
            continue;
        }

        WriterFrameQueue* aq = SparseSet_WriterFrameQueue_value_at(&data->encrypt_qs, fd);
        writer_queue_drain(aq);
        WriterFrameQueue_free(aq);
        if (SparseSet_WriterFrameQueue_contains(&data->encrypt_qs, fd)) {
            SparseSet_WriterFrameQueue_erase(&data->encrypt_qs, fd);
        }

        AuthFrameQueue* dq = SparseSet_AuthFrameQueue_value_at(&data->decrypt_qs, fd);
        auth_queue_drain(dq);
        AuthFrameQueue_free(dq);
        if (SparseSet_AuthFrameQueue_contains(&data->decrypt_qs, fd)) {
            SparseSet_AuthFrameQueue_erase(&data->decrypt_qs, fd);
        }

        memset(SparseSet_AuthEntry_get(&data->entries, fd), 0, sizeof(AuthEntry));
        SparseSet_AuthEntry_erase(&data->entries, fd);
    }
}

static int handshake_or_decrypt_frame(AuthData* data, AuthEntry* entry, const ReaderFrame* frame, size_t fd,
                                      SparseSet_AuthFrameQueue* m_decrypted_out,
                                      SparseSet_WriterFrameQueue* handshake_out) {
    switch (entry->auth_state) {
    case AUTH_NONCE: {
        uint32_t sig_len;
        unsigned char* sig = tetrish_server_sign_nonce(frame->content.ptr, (uint32_t)frame->content.length, data->credential.private_key, &sig_len);
        if (sig == NULL) {
            return -1;
        }
        if (sig_len == 0 || sig_len > FRAME_MAX) {
            free(sig);
            return -1;
        }

        unsigned char* cert_copy = malloc(data->credential.certificate_len);
        if (cert_copy == NULL) {
            free(sig);
            return -1;
        }
        memcpy(cert_copy, data->credential.certificate, data->credential.certificate_len);

        WriterFrameQueue* wq = SparseSet_WriterFrameQueue_activate(handshake_out, fd);
        const WriterFrame sig_frame = { sig, sig_len };
        if (WriterFrameQueue_push_back(wq, &sig_frame) == -1) {
            free(sig);
            free(cert_copy);
            return -1;
        }
        const WriterFrame cert_frame = { cert_copy, data->credential.certificate_len };
        if (WriterFrameQueue_push_back(wq, &cert_frame) == -1) {
            free(cert_copy);
            return -1;
        }
        entry->auth_state = AUTH_SYMKEY;
        return 0;
    }
    case AUTH_SYMKEY: {
        uint32_t key_len;
        unsigned char* session_key = tetrish_server_decrypt_session_key(frame->content.ptr, (uint32_t)frame->content.length, &data->credential, &key_len);
        if (session_key == NULL || key_len != SESSION_KEY_LEN) {
            free(session_key);
            return -1;
        }
        memcpy(entry->key, session_key, SESSION_KEY_LEN);
        free(session_key);
        entry->auth_state = AUTH_DONE;
        return 0;
    }
    case AUTH_DONE: {
        uint32_t plain_len;
        unsigned char* plain = tetrish_session_decrypt(&entry->key, frame->content.ptr, (uint32_t)frame->content.length, &plain_len);
        AuthFrame decrypted;
        if (plain == NULL) {
            decrypted = (AuthFrame){
                .status = FRAME_DECRYPT_ERROR,
            };
        }
        else {
            decrypted = (AuthFrame){
                 { plain, plain_len },
                FRAME_OK,
            };
        }
        AuthFrameQueue* out = SparseSet_AuthFrameQueue_value_at(m_decrypted_out, fd);
        const int err = AuthFrameQueue_push_back(out, &decrypted);
        // overflow violates the capacity contract (see server_tick's accept
        // fan-out); the fd is failed
        assert(err != -1);
        if (err == -1) {
            free(plain);
            return -1;
        }
        SparseSet_AuthFrameQueue_activate(m_decrypted_out, fd);
        return 0;
    }
    }
    assert(false);
    return -1;
}

void AuthData_handshake_or_decrypt(AuthData* data, const SparseSet_ReaderFrameQueue* read_qs,
                               SparseSet_AuthFrameQueue* m_decrypted_out,
                               SparseSet_WriterFrameQueue* handshake_out,
                               SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_ReaderFrameQueue_size(read_qs); i++) {
        const size_t fd = SparseSet_ReaderFrameQueue_key_at_idx(read_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "read_qs fd must not be in err_fds");
            continue;
        }
        ReaderFrameQueue* q = SparseSet_ReaderFrameQueue_at_idx(read_qs, i);
        AuthEntry* entry = SparseSet_AuthEntry_get(&data->entries, fd);

        bool failed = false;
        const size_t frame_count = ReaderFrameQueue_size(q);
        for (size_t j = 0; j < frame_count; j++) {
            const ReaderFrame* frame = ReaderFrameQueue_at(q, j);

            if (frame->status != READER_FRAME_OK) {
                if (entry->auth_state != AUTH_DONE) {
                    failed = true;
                    break;
                }
                // TODO: make a function to convert error
                const AuthFrame forwarded = {.status = FRAME_PAYLOAD_TOO_LARGE};
                AuthFrameQueue* out = SparseSet_AuthFrameQueue_value_at(m_decrypted_out, fd);
                const int err = AuthFrameQueue_push_back(out, &forwarded);
                // overflow violates the capacity contract (see server_tick's
                // accept fan-out); the fd is failed
                assert(err != -1);
                if (err == -1) {
                    failed = true;
                    break;
                }
                SparseSet_AuthFrameQueue_activate(m_decrypted_out, fd);
                continue;
            }
            if (handshake_or_decrypt_frame(data, entry, frame, fd, m_decrypted_out, handshake_out) == -1) {
                failed = true;
                break;
            }
        }

        if (failed) {
            *SparseSet_bool_activate(err_fds, fd) = true;
            if (SparseSet_AuthFrameQueue_contains(m_decrypted_out, fd)) {
                auth_queue_drain(SparseSet_AuthFrameQueue_get(m_decrypted_out, fd));
                SparseSet_AuthFrameQueue_erase(m_decrypted_out, fd);
            }
            if (SparseSet_WriterFrameQueue_contains(handshake_out, fd)) {
                writer_queue_drain(SparseSet_WriterFrameQueue_get(handshake_out, fd));
                SparseSet_WriterFrameQueue_erase(handshake_out, fd);
            }
        }
    }
}

void AuthData_encrypt(AuthData* data, const SparseSet_WriterFrameQueue* m_encrypt_qs, SparseSet_WriterFrameQueue* out, SparseSet_bool* err_fds) {
    for (size_t i = 0; i < SparseSet_WriterFrameQueue_size(m_encrypt_qs); i++) {
        const size_t fd = SparseSet_WriterFrameQueue_key_at_idx(m_encrypt_qs, i);
        if (SparseSet_bool_contains(err_fds, fd)) {
            assert(false && "m_encrypt_qs fd must not be in err_fds");
            continue;
        }
        WriterFrameQueue* q = SparseSet_WriterFrameQueue_at_idx(m_encrypt_qs, i);
        AuthEntry* entry = SparseSet_AuthEntry_get(&data->entries, fd);

        const size_t frame_count = WriterFrameQueue_size(q);
        for (size_t j = 0; j < frame_count; j++) {
            const WriterFrame* frame = WriterFrameQueue_at(q, j);

            uint32_t cipher_len;
            unsigned char* cipher = tetrish_session_encrypt(&entry->key, frame->ptr, (uint32_t)frame->length, &cipher_len);
            if (cipher == NULL) {
                *SparseSet_bool_activate(err_fds, fd) = true;
                break;
            }
            if (cipher_len == 0 || cipher_len > FRAME_MAX) {
                assert(false);
                free(cipher);
                *SparseSet_bool_activate(err_fds, fd) = true;
                break;
            }

            WriterFrameQueue* wq = SparseSet_WriterFrameQueue_activate(out, fd);
            const WriterFrame cipher_frame = { cipher, cipher_len };
            if (WriterFrameQueue_push_back(wq, &cipher_frame) == -1) {
                // Dropped, not an error: the client's timeout will retransmit the request.
                // TODO: proper backpressure belongs in the application layer, which would
                // have to see write-queue occupancy to stop producing responses; once it
                // does, this becomes an assert that the queue is never full here.
                LOGGER_LOG(LOG_WARN, "auth", "write queue full, dropping response for fd=%zu", fd);
                free(cipher);
            }
        }
    }
}
