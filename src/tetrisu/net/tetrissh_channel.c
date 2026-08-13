#include "net/tetrissh_channel.h"

#include "wire.h"

#include <errno.h>
#include <limits.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OUTPUT_NONE,
    OUTPUT_NONCE,
    OUTPUT_SESSION_KEY,
    OUTPUT_APPLICATION,
};

static unsigned char* encrypt_payload_quiet(
    const SessionKey* key,
    const unsigned char* plaintext,
    size_t plaintext_length,
    size_t* encrypted_length
) {
    if (plaintext_length > INT_MAX || plaintext_length > SIZE_MAX - AES_BLOCK ||
        plaintext_length + AES_BLOCK > SIZE_MAX - AES_IV_LEN - HMAC_LEN) {
        return NULL;
    }
    unsigned char iv[AES_IV_LEN];
    if (RAND_bytes(iv, AES_IV_LEN) != 1) {
        return NULL;
    }

    const size_t capacity = AES_IV_LEN + plaintext_length + AES_BLOCK + HMAC_LEN;
    unsigned char* output = malloc(capacity);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (output == NULL || context == NULL) {
        free(output);
        EVP_CIPHER_CTX_free(context);
        return NULL;
    }
    memcpy(output, iv, AES_IV_LEN);

    int ciphertext_length = 0;
    int final_length = 0;
    const unsigned char* aes_key = *key + HMAC_KEY_LEN;
    if (EVP_EncryptInit_ex(context, EVP_aes_128_cbc(), NULL, aes_key, iv) != 1 ||
        EVP_EncryptUpdate(
            context,
            output + AES_IV_LEN,
            &ciphertext_length,
            plaintext,
            (int)plaintext_length
        ) != 1 ||
        EVP_EncryptFinal_ex(
            context,
            output + AES_IV_LEN + ciphertext_length,
            &final_length
        ) != 1) {
        EVP_CIPHER_CTX_free(context);
        free(output);
        return NULL;
    }
    EVP_CIPHER_CTX_free(context);

    const size_t ciphertext_total = (size_t)ciphertext_length + (size_t)final_length;
    unsigned int mac_length = 0;
    if (HMAC(
        EVP_sha256(),
        *key,
        HMAC_KEY_LEN,
        output,
        AES_IV_LEN + ciphertext_total,
        output + AES_IV_LEN + ciphertext_total,
        &mac_length
    ) == NULL || mac_length != HMAC_LEN) {
        free(output);
        return NULL;
    }
    *encrypted_length = AES_IV_LEN + ciphertext_total + HMAC_LEN;
    return output;
}

static unsigned char* decrypt_payload_quiet(
    const SessionKey* key,
    const unsigned char* token,
    size_t token_length,
    size_t* plaintext_length
) {
    if (token_length < AES_IV_LEN + AES_BLOCK + HMAC_LEN) {
        return NULL;
    }
    const size_t ciphertext_length = token_length - AES_IV_LEN - HMAC_LEN;
    if (ciphertext_length > INT_MAX) {
        return NULL;
    }

    unsigned char computed_mac[HMAC_LEN];
    unsigned int mac_length = 0;
    if (HMAC(
        EVP_sha256(),
        *key,
        HMAC_KEY_LEN,
        token,
        AES_IV_LEN + ciphertext_length,
        computed_mac,
        &mac_length
    ) == NULL || mac_length != HMAC_LEN ||
        CRYPTO_memcmp(
            computed_mac,
            token + AES_IV_LEN + ciphertext_length,
            HMAC_LEN
        ) != 0) {
        return NULL;
    }

    unsigned char* plaintext = malloc(ciphertext_length);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (plaintext == NULL || context == NULL) {
        free(plaintext);
        EVP_CIPHER_CTX_free(context);
        return NULL;
    }
    int decrypted_length = 0;
    int final_length = 0;
    const unsigned char* aes_key = *key + HMAC_KEY_LEN;
    if (EVP_DecryptInit_ex(context, EVP_aes_128_cbc(), NULL, aes_key, token) != 1 ||
        EVP_DecryptUpdate(
            context,
            plaintext,
            &decrypted_length,
            token + AES_IV_LEN,
            (int)ciphertext_length
        ) != 1 ||
        EVP_DecryptFinal_ex(context, plaintext + decrypted_length, &final_length) != 1) {
        EVP_CIPHER_CTX_free(context);
        free(plaintext);
        return NULL;
    }
    EVP_CIPHER_CTX_free(context);
    *plaintext_length = (size_t)decrypted_length + (size_t)final_length;
    return plaintext;
}

static int queue_frame(TetrisshChannel* channel, const void* payload, size_t length, unsigned kind) {
    if (length == 0 || length > FRAME_MAX || length > UINT32_MAX || channel->output.ptr != NULL) {
        return -1;
    }
    unsigned char* wire = malloc(sizeof(uint32_t) + length);
    if (wire == NULL) {
        return -1;
    }
    encode_u32_be(wire, (uint32_t)length);
    memcpy(wire + sizeof(uint32_t), payload, length);
    channel->output.ptr = wire;
    channel->output.len = sizeof(uint32_t) + length;
    channel->output_used = 0;
    channel->output_kind = kind;
    return 0;
}

static void input_reset(TetrisshChannel* channel) {
    channel->input_length_used = 0;
    owned_bytes_free(&channel->input_body);
    channel->input_body_used = 0;
}

static int fail(TetrisshChannel* channel, SecureChannelStep* step, ClientError error) {
    channel->state = SECURE_CHANNEL_FAILED;
    step->events |= SECURE_CHANNEL_EVENT_ERROR;
    step->error = error;
    return -1;
}

static int read_some(SocketTransport* transport, void* destination, size_t wanted, size_t* used) {
    while (*used < wanted) {
        const ssize_t received = socket_transport_read(
            transport,
            (unsigned char*)destination + *used,
            wanted - *used
        );
        if (received > 0) {
            *used += (size_t)received;
            continue;
        }
        if (received == 0) {
            return -2;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    return 1;
}

static int read_frame(TetrisshChannel* channel, SocketTransport* transport, OwnedBytes* frame) {
    int result = read_some(
        transport,
        channel->input_length,
        sizeof(channel->input_length),
        &channel->input_length_used
    );
    if (result <= 0) {
        return result;
    }

    if (channel->input_body.ptr == NULL) {
        const uint32_t length = decode_u32_be(channel->input_length);
        if (length == 0 || length > FRAME_MAX) {
            return -3;
        }
        channel->input_body.ptr = malloc(length);
        if (channel->input_body.ptr == NULL) {
            return -4;
        }
        channel->input_body.len = length;
    }

    result = read_some(
        transport,
        channel->input_body.ptr,
        channel->input_body.len,
        &channel->input_body_used
    );
    if (result <= 0) {
        return result;
    }

    owned_bytes_move(frame, &channel->input_body);
    channel->input_length_used = 0;
    channel->input_body_used = 0;
    return 1;
}

static X509* parse_certificate(const OwnedBytes* bytes) {
    if (bytes->len > INT_MAX) {
        return NULL;
    }
    BIO* input = BIO_new_mem_buf(bytes->ptr, (int)bytes->len);
    if (input == NULL) {
        return NULL;
    }
    X509* certificate = PEM_read_bio_X509(input, NULL, NULL, NULL);
    BIO_free(input);
    return certificate;
}

static X509* read_ca_certificate(const char* path) {
    FILE* input = fopen(path, "rb");
    if (input == NULL) {
        return NULL;
    }
    X509* certificate = PEM_read_X509(input, NULL, NULL, NULL);
    fclose(input);
    return certificate;
}

static bool verify_certificate(X509* certificate, const char* ca_path) {
    bool valid = false;
    X509* ca = read_ca_certificate(ca_path);
    X509_STORE* store = X509_STORE_new();
    X509_STORE_CTX* context = X509_STORE_CTX_new();
    if (ca != NULL && store != NULL && context != NULL &&
        X509_STORE_add_cert(store, ca) == 1 &&
        X509_STORE_CTX_init(context, store, certificate, NULL) == 1 &&
        X509_verify_cert(context) == 1) {
        valid = true;
    }
    X509_STORE_CTX_free(context);
    X509_STORE_free(store);
    X509_free(ca);
    return valid;
}

static bool verify_nonce_signature(
    X509* certificate,
    const OwnedBytes* proof,
    const unsigned char nonce[NONCE_LEN]
) {
    bool valid = false;
    EVP_PKEY* public_key = X509_get_pubkey(certificate);
    EVP_MD_CTX* digest = EVP_MD_CTX_new();
    EVP_PKEY_CTX* key_context = NULL;
    if (public_key != NULL && digest != NULL &&
        EVP_DigestVerifyInit(digest, &key_context, EVP_sha256(), NULL, public_key) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) == 1 &&
        EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_MAX) == 1 &&
        EVP_DigestVerifyUpdate(digest, nonce, NONCE_LEN) == 1 &&
        EVP_DigestVerifyFinal(digest, proof->ptr, proof->len) == 1) {
        valid = true;
    }
    EVP_MD_CTX_free(digest);
    EVP_PKEY_free(public_key);
    return valid;
}

static int encrypt_session_key(X509* certificate, const SessionKey* key, OwnedBytes* encrypted) {
    EVP_PKEY* public_key = X509_get_pubkey(certificate);
    EVP_PKEY_CTX* context = public_key == NULL ? NULL : EVP_PKEY_CTX_new(public_key, NULL);
    size_t length = 0;
    int result = -1;
    if (context != NULL &&
        EVP_PKEY_encrypt_init(context) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_OAEP_PADDING) == 1 &&
        EVP_PKEY_CTX_set_rsa_oaep_md(context, EVP_sha256()) == 1 &&
        EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha256()) == 1 &&
        EVP_PKEY_encrypt(context, NULL, &length, *key, SESSION_KEY_LEN) == 1 &&
        length > 0 && length <= FRAME_MAX) {
        encrypted->ptr = malloc(length);
        if (encrypted->ptr != NULL &&
            EVP_PKEY_encrypt(context, encrypted->ptr, &length, *key, SESSION_KEY_LEN) == 1) {
            encrypted->len = length;
            result = 0;
        } else {
            owned_bytes_free(encrypted);
        }
    }
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(public_key);
    return result;
}

static int accept_certificate(
    TetrisshChannel* channel,
    OwnedBytes* frame,
    SecureChannelStep* step
) {
    X509* certificate = parse_certificate(frame);
    if (certificate == NULL || !verify_certificate(certificate, channel->ca_path)) {
        X509_free(certificate);
        return fail(channel, step, client_error(CLIENT_ERROR_CERTIFICATE, 0, "server certificate rejected"));
    }
    if (!verify_nonce_signature(certificate, &channel->proof, channel->nonce)) {
        X509_free(certificate);
        return fail(channel, step, client_error(CLIENT_ERROR_HANDSHAKE, 0, "nonce signature rejected"));
    }
    if (RAND_bytes(channel->key, SESSION_KEY_LEN) != 1) {
        X509_free(certificate);
        return fail(channel, step, client_error(CLIENT_ERROR_HANDSHAKE, 0, "cannot generate session key"));
    }

    OwnedBytes encrypted;
    owned_bytes_init(&encrypted);
    if (encrypt_session_key(certificate, &channel->key, &encrypted) == -1 ||
        queue_frame(channel, encrypted.ptr, encrypted.len, OUTPUT_SESSION_KEY) == -1) {
        owned_bytes_free(&encrypted);
        X509_free(certificate);
        return fail(channel, step, client_error(CLIENT_ERROR_HANDSHAKE, 0, "cannot encrypt session key"));
    }
    owned_bytes_free(&encrypted);
    X509_free(certificate);
    owned_bytes_free(&channel->proof);
    channel->state = SECURE_CHANNEL_SEND_SESSION_KEY;
    return 0;
}

static int flush_output(
    TetrisshChannel* channel,
    SocketTransport* transport,
    SecureChannelStep* step
) {
    while (channel->output_used < channel->output.len) {
        const ssize_t sent = socket_transport_write(
            transport,
            channel->output.ptr + channel->output_used,
            channel->output.len - channel->output_used
        );
        if (sent > 0) {
            channel->output_used += (size_t)sent;
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        const int code = sent == 0 ? EPIPE : errno;
        return fail(channel, step, client_error(CLIENT_ERROR_TRANSPORT, code, "socket write failed"));
    }

    const unsigned completed_kind = channel->output_kind;
    owned_bytes_free(&channel->output);
    channel->output_used = 0;
    channel->output_kind = OUTPUT_NONE;
    if (completed_kind == OUTPUT_NONCE) {
        channel->state = SECURE_CHANNEL_RECV_PROOF;
    } else if (completed_kind == OUTPUT_SESSION_KEY) {
        channel->state = SECURE_CHANNEL_READY;
        step->events |= SECURE_CHANNEL_EVENT_HANDSHAKE_READY;
    } else if (completed_kind == OUTPUT_APPLICATION) {
        step->events |= SECURE_CHANNEL_EVENT_APP_SENT;
    }
    return 0;
}

void tetrissh_channel_init(TetrisshChannel* channel, const char* ca_path) {
    memset(channel, 0, sizeof(*channel));
    channel->state = SECURE_CHANNEL_NEW;
    channel->ca_path = ca_path;
}

void tetrissh_channel_free(TetrisshChannel* channel) {
    owned_bytes_free(&channel->proof);
    owned_bytes_free(&channel->input_body);
    owned_bytes_free(&channel->output);
    OPENSSL_cleanse(channel->nonce, sizeof(channel->nonce));
    OPENSSL_cleanse(channel->key, sizeof(channel->key));
    const char* ca_path = channel->ca_path;
    memset(channel, 0, sizeof(*channel));
    channel->state = SECURE_CHANNEL_NEW;
    channel->ca_path = ca_path;
}

int tetrissh_channel_start(TetrisshChannel* channel, ClientError* error) {
    if (channel->state != SECURE_CHANNEL_NEW || RAND_bytes(channel->nonce, NONCE_LEN) != 1 ||
        queue_frame(channel, channel->nonce, NONCE_LEN, OUTPUT_NONCE) == -1) {
        channel->state = SECURE_CHANNEL_FAILED;
        *error = client_error(CLIENT_ERROR_HANDSHAKE, 0, "cannot start handshake");
        return -1;
    }
    channel->state = SECURE_CHANNEL_SEND_NONCE;
    return 0;
}

unsigned tetrissh_channel_want(const TetrisshChannel* channel) {
    unsigned wanted = 0;
    if (channel->output.ptr != NULL) {
        wanted |= SECURE_CHANNEL_WANT_WRITE;
    }
    if (channel->state == SECURE_CHANNEL_RECV_PROOF ||
        channel->state == SECURE_CHANNEL_RECV_CERTIFICATE ||
        channel->state == SECURE_CHANNEL_READY) {
        wanted |= SECURE_CHANNEL_WANT_READ;
    }
    return wanted;
}

int tetrissh_channel_submit(
    TetrisshChannel* channel,
    const unsigned char* plaintext,
    size_t length,
    ClientError* error
) {
    if (channel->state != SECURE_CHANNEL_READY || channel->output.ptr != NULL ||
        length > UINT32_MAX || (length != 0 && plaintext == NULL)) {
        *error = client_error(CLIENT_ERROR_PROTOCOL, 0, "secure channel is not idle");
        return -1;
    }
    size_t encrypted_length = 0;
    unsigned char* encrypted = encrypt_payload_quiet(
        &channel->key,
        plaintext,
        length,
        &encrypted_length
    );
    if (encrypted == NULL || queue_frame(channel, encrypted, encrypted_length, OUTPUT_APPLICATION) == -1) {
        free(encrypted);
        *error = client_error(CLIENT_ERROR_INTERNAL, 0, "cannot encrypt message");
        return -1;
    }
    free(encrypted);
    return 0;
}

int tetrissh_channel_step(
    TetrisshChannel* channel,
    SocketTransport* transport,
    bool readable,
    bool writable,
    SecureChannelStep* step
) {
    memset(step, 0, sizeof(*step));
    if (writable && channel->output.ptr != NULL && flush_output(channel, transport, step) == -1) {
        return -1;
    }
    if (!readable || (channel->state != SECURE_CHANNEL_RECV_PROOF &&
        channel->state != SECURE_CHANNEL_RECV_CERTIFICATE &&
        channel->state != SECURE_CHANNEL_READY)) {
        return 0;
    }

    OwnedBytes frame;
    owned_bytes_init(&frame);
    const int read_result = read_frame(channel, transport, &frame);
    if (read_result == 0) {
        return 0;
    }
    if (read_result == -2) {
        step->events |= SECURE_CHANNEL_EVENT_CLOSED;
        return 0;
    }
    if (read_result < 0) {
        input_reset(channel);
        ClientError error;
        if (read_result == -4) {
            error = client_error(CLIENT_ERROR_NOMEM, 0, "cannot allocate input frame");
        } else if (read_result == -3) {
            error = client_error(CLIENT_ERROR_PROTOCOL, 0, "invalid secure-frame length");
        } else {
            error = client_error(CLIENT_ERROR_TRANSPORT, errno, "socket read failed");
        }
        return fail(channel, step, error);
    }

    if (channel->state == SECURE_CHANNEL_RECV_PROOF) {
        owned_bytes_move(&channel->proof, &frame);
        channel->state = SECURE_CHANNEL_RECV_CERTIFICATE;
    } else if (channel->state == SECURE_CHANNEL_RECV_CERTIFICATE) {
        const int result = accept_certificate(channel, &frame, step);
        owned_bytes_free(&frame);
        return result;
    } else {
        size_t plaintext_length = 0;
        unsigned char* plaintext = decrypt_payload_quiet(
            &channel->key,
            frame.ptr,
            frame.len,
            &plaintext_length
        );
        owned_bytes_free(&frame);
        if (plaintext == NULL) {
            return fail(channel, step, client_error(CLIENT_ERROR_DECRYPT, 0, "authenticated frame rejected"));
        }
        step->plaintext.ptr = plaintext;
        step->plaintext.len = plaintext_length;
        step->events |= SECURE_CHANNEL_EVENT_PLAINTEXT;
    }
    return 0;
}

void tetrissh_channel_step_free(SecureChannelStep* step) {
    owned_bytes_free(&step->plaintext);
    step->events = SECURE_CHANNEL_EVENT_NONE;
    step->error = client_error(CLIENT_ERROR_NONE, 0, NULL);
}
