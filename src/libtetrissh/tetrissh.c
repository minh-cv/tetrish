#include "tetrissh.h"
#include "dtor.h"
#include <openssl/evp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

DTOR_WRAPPER_DEFINE(free)
DTOR_WRAPPER_DEFINE(X509_free)
DTOR_WRAPPER_DEFINE(EVP_PKEY_free)
DTOR_WRAPPER_DEFINE(fclose)

uint32_t decode_u32_be(const uint8_t buf[4]) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           ((uint32_t)buf[3] << 0);
}

void encode_u32_be(uint8_t buf[4], uint32_t value) {
    buf[0] = (uint8_t)((value >> 24) & 0xFF);
    buf[1] = (uint8_t)((value >> 16) & 0xFF);
    buf[2] = (uint8_t)((value >> 8) & 0xFF);
    buf[3] = (uint8_t)((value >> 0) & 0xFF);
}

static unsigned char* store_file(const char* path, uint32_t* len) {
    DTOR_DEFINE(dtor, 10);
    FILE* cert_file = fopen(path, "rb");
    if (cert_file == NULL) {
        return NULL;
    }
    DTOR_INSERT(dtor, fclose, cert_file);
    
    if (fseek(cert_file, 0, SEEK_END) != 0) {
        DTOR_RETURN(dtor, NULL);
    }

    long len_long = ftell(cert_file);
    if (len_long <= 0 || len_long > UINT32_MAX) {
        DTOR_RETURN(dtor, NULL);
    }
    *len = (uint32_t)len_long;

    if (fseek(cert_file, 0, SEEK_SET) != 0) {
        DTOR_RETURN(dtor, NULL);
    }

    unsigned char* cert_buf = malloc(*len);
    if (cert_buf == NULL) {
        DTOR_RETURN(dtor, NULL);
    }

    if (fread(cert_buf, 1, *len, cert_file) != *len) {
        free(cert_buf);
        DTOR_RETURN(dtor, NULL);
    }

    DTOR_RETURN(dtor, cert_buf);
}


int tetrish_credential_init(TetrishCredential* buf, const char* key_path, const char* certificate_path) {
    buf->private_key = load_private_key(key_path);
    if (buf->private_key == NULL) {
        return -1;
    }

    buf->certificate = store_file(certificate_path, &buf->certificate_len);

    if (buf->certificate == NULL) {
        EVP_PKEY_free(buf->private_key);
        return -1;
    }
    return 0;
}

void tetrish_credential_free(TetrishCredential *buf) {
    free(buf->certificate);
    EVP_PKEY_free(buf->private_key);
}

static int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t recvd = 0;

    while (recvd < len) {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n == 0) {
            return -1;
        }
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        recvd += (size_t)n;
    }

    return 0;
}

static int send_uint32_t(int sockfd, uint32_t value) {
    uint8_t buf[4];
    encode_u32_be(buf, value);
    return send_all(sockfd, buf, sizeof(uint32_t));
}

static int recv_uint32_t(int sockfd, uint32_t* value) {
    uint8_t buf[4];
    if (recv_all(sockfd, buf, sizeof(buf)) == -1) {
        return -1;
    }
    *value = decode_u32_be(buf);
    return 0;
}

int tetrish_client_handshake(int sockfd, const char* ca_path, SessionKey* session_key) {
    DTOR_DEFINE(dtor, 10);

    unsigned char nonce_buf[SESSION_KEY_LEN];    
    if (
        generate_session_key(nonce_buf) == -1 || 
        send_uint32_t(sockfd, SESSION_KEY_LEN) == -1 ||
        send_all(sockfd, nonce_buf, SESSION_KEY_LEN) == -1) {
            DTOR_RETURN(dtor, -1);
        }
    
    uint32_t signed_nonce_len;
    if (recv_uint32_t(sockfd, &signed_nonce_len) == -1 || signed_nonce_len > FRAME_MAX) DTOR_RETURN(dtor, -1);

    unsigned char* signed_nonce_buf = read_bytes(sockfd, signed_nonce_len);
    if (signed_nonce_buf == NULL) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, signed_nonce_buf);

    uint32_t cert_len;
    if (recv_uint32_t(sockfd, &cert_len) == -1 || cert_len > FRAME_MAX) DTOR_RETURN(dtor, -1);

    unsigned char* cert_buf = read_bytes(sockfd, cert_len);
    if (cert_buf == NULL) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, cert_buf);

    X509* cert = load_cert_bytes(cert_buf, (int)cert_len);
    if (cert == NULL) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, X509_free, cert);
    int verify_cert = verify_server_cert(cert, ca_path);

    if (verify_cert == 0) {
        fprintf(stderr, "Unable to verify certificate.\n");
        DTOR_RETURN(dtor, -1);
    }

    int verify_message = verify_message_pss(cert, signed_nonce_buf, signed_nonce_len, nonce_buf, sizeof(nonce_buf));

    if (verify_message == 0) {
        fprintf(stderr, "Unable to verify signed nonce.\n");
        DTOR_RETURN(dtor, -1);
    }

    if (generate_session_key(*session_key) == -1) {
        DTOR_RETURN(dtor, -1);
    }

    EVP_PKEY* public_key = X509_get_pubkey(cert);
    DTOR_INSERT(dtor, EVP_PKEY_free, public_key);

    size_t encrypted_shared_key_len;
    unsigned char* encrypted_shared_key = rsa_encrypt_block(public_key, *session_key, SESSION_KEY_LEN, &encrypted_shared_key_len, 1);
    DTOR_INSERT(dtor, free, encrypted_shared_key);
    if (encrypted_shared_key == NULL || encrypted_shared_key_len > FRAME_MAX) {
        DTOR_RETURN(dtor, -1);
    }

    if (send_uint32_t(sockfd, (uint32_t)encrypted_shared_key_len) == -1 ||
    send_all(sockfd, encrypted_shared_key, encrypted_shared_key_len) == -1) {
        DTOR_RETURN(dtor, -1);
    }
    
    DTOR_RETURN(dtor, 0);
}

unsigned char* tetrish_server_sign_nonce(unsigned char* nonce, uint32_t nonce_length, EVP_PKEY* private_key, uint32_t* response_length) {
    size_t sig_len;
    unsigned char* signed_nonce = sign_message_pss(private_key, nonce, nonce_length, &sig_len);
    if (signed_nonce == NULL || sig_len > FRAME_MAX) {
        fprintf(stderr, "Failed to sign nonce\n");
        return NULL;
    }
    *response_length = (uint32_t)sig_len;
    return signed_nonce;
}

unsigned char* tetrish_server_decrypt_session_key(const unsigned char* cipherkey, uint32_t cipherkey_len, TetrishCredential* info, uint32_t* response_length) {
    size_t shared_key_len;
    unsigned char* shared_key_buf = rsa_decrypt_block(info->private_key, cipherkey, cipherkey_len, &shared_key_len, 1);
    if (shared_key_buf == NULL) {
        return NULL;
    }

    if (shared_key_len != SESSION_KEY_LEN) {
        fprintf(stderr, "Cannot parse session key\n");
        free(shared_key_buf);
        return NULL;
    }

    *response_length = (uint32_t)shared_key_len;
    
    return shared_key_buf;
}

unsigned char* tetrish_recv_frame(int fd, uint32_t* plaintext_length, SessionKey* key) {
    uint32_t encrypted_length;
    if (recv_uint32_t(fd, &encrypted_length) == -1 || encrypted_length > FRAME_MAX) {
        return NULL;
    }

    unsigned char* encrypted_msg = malloc(encrypted_length);
    if (encrypted_msg == NULL) {
        return NULL;
    }

    if (recv_all(fd, encrypted_msg, encrypted_length) == -1) {
        free(encrypted_msg);
        return NULL;
    }

    if (key == NULL) {
        *plaintext_length = encrypted_length;
        return encrypted_msg;
    }

    unsigned char* msg = tetrish_session_decrypt(key, encrypted_msg, encrypted_length, plaintext_length);
    if (msg == NULL) {
        free(encrypted_msg);
        return NULL;
    }

    free(encrypted_msg);
    return msg;
}

int tetrish_send_frame(int sockfd, const unsigned char* plaintext, uint32_t plaintext_length, SessionKey* key) {
    if (key == NULL) {
        if (plaintext_length > FRAME_MAX || send_uint32_t(sockfd, (uint32_t)plaintext_length) == -1 ||
        send_all(sockfd, plaintext, plaintext_length) == -1) {
                return -1;
        }

        return 0;
    }

    DTOR_DEFINE(dtor, 10);
    uint32_t encrypted_length;
    unsigned char* encrypted_msg = tetrish_session_encrypt(key, plaintext, plaintext_length, &encrypted_length);
    DTOR_INSERT(dtor, free, encrypted_msg);
    if (encrypted_msg == NULL) {
        DTOR_RETURN(dtor, -1);
    }

    if (send_uint32_t(sockfd, (uint32_t)encrypted_length) == -1 ||
        send_all(sockfd, encrypted_msg, encrypted_length) == -1) {
            DTOR_RETURN(dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

unsigned char* tetrish_session_encrypt(SessionKey* key, const unsigned char *plaintext, uint32_t plaintext_length, uint32_t *out_len) {
    size_t encrypted_length;
    unsigned char* encrypted_msg = session_encrypt(*key, plaintext, plaintext_length, &encrypted_length);
    if (encrypted_msg == NULL || encrypted_length > FRAME_MAX) {
        free(encrypted_msg);
        return NULL;
    }
    *out_len = (uint32_t)encrypted_length;
    return encrypted_msg;
}

unsigned char* tetrish_session_decrypt(SessionKey* key, const unsigned char *encrypted_msg, uint32_t encrypted_length, uint32_t *out_len) {
    size_t length;
    unsigned char* msg = session_decrypt(*key, encrypted_msg, encrypted_length, &length);
    if (msg == NULL || length > FRAME_MAX) {
        free(msg);
        return NULL;
    }

    *out_len = (uint32_t)length;
    return msg;
}