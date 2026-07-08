#ifndef TETRISH_SSH_H
#define TETRISH_SSH_H

#include "common.h"
#include <openssl/types.h>
#include <stdint.h>

#define FRAME_MAX (64u * 1024u)
#define FRAME_LENGTH 4u

typedef struct TetrishCredential {
    EVP_PKEY* private_key;
    unsigned char* certificate;
    uint32_t certificate_len;
} TetrishCredential;

typedef unsigned char SessionKey[SESSION_KEY_LEN];

int tetrish_credential_init(TetrishCredential* buf, const char* key_path, const char* certificate_path);
void tetrish_credential_free(TetrishCredential* buf);

uint32_t decode_u32_be(const uint8_t buf[4]);
void encode_u32_be(uint8_t buf[4], uint32_t value);

int tetrish_client_handshake(int sockfd, const char* ca_path, SessionKey* session_key);
unsigned char* tetrish_server_make_auth_response(unsigned char* nonce, uint32_t nonce_length, TetrishCredential* info, uint32_t* response_length);
unsigned char* tetrish_server_decrypt_session_key(const unsigned char* cipherkey, uint32_t cipherkey_len, TetrishCredential* info, uint32_t* response_length);

unsigned char* tetrish_recv_frame(int fd, uint32_t* plaintext_length, SessionKey* key);
int tetrish_send_frame(int sockfd, const unsigned char* plaintext, uint32_t plaintext_length, SessionKey* key);

#endif
