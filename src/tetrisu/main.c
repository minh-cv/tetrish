#include "common.h"
#include "htttp.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/x509.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <dtor.h>

#define MAX_MESSAGE_SIZE 4096

static DTOR_WRAPPER_DEFINE(htttp_message_free)
static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(X509_free)
static DTOR_WRAPPER_DEFINE(EVP_PKEY_free)

static uint32_t decode_u32_be(const uint8_t buf[4]) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           ((uint32_t)buf[3] << 0);
}

static void encode_u32_be(uint8_t buf[4], uint32_t value) {
    buf[0] = (uint8_t)((value >> 24) & 0xFF);
    buf[1] = (uint8_t)((value >> 16) & 0xFF);
    buf[2] = (uint8_t)((value >> 8) & 0xFF);
    buf[3] = (uint8_t)((value >> 0) & 0xFF);
}

static int send_uint32_t(int sockfd, uint32_t value) {
    uint8_t buf[4];
    encode_u32_be(buf, value);
    return send_all(sockfd, buf, sizeof(uint32_t));
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


static int recv_uint32_t(int sockfd, uint32_t* value) {
    uint8_t buf[4];
    if (recv_all(sockfd, buf, sizeof(buf)) == -1) {
        return -1;
    }
    *value = decode_u32_be(buf);
    return 0;
}

int htttp_loop(int fd, const char* buf, size_t buf_len, unsigned char(*shared_key)[SESSION_KEY_LEN]) {
    DTOR_DEFINE(dtor, 10);
    
    htttp_message_t message;
    htttp_message_init(&message);
    message.kind = HTTTP_REQUEST;
    strncpy(message.path, "/insert/path/here", HTTTP_MAX_PATH);
    strncpy(message.method, "SOMEMETHOD", HTTTP_MAX_METHOD);

    char val_buf[32] = {0};
    int val_buf_written = snprintf(val_buf, sizeof(val_buf), "%zu", buf_len);
    if ((size_t)val_buf_written >= sizeof(val_buf) || val_buf_written < 0) {
        fprintf(stderr, "Message too big\n");
        DTOR_RETURN(dtor, -1);
    }

    htttp_add_header(&message, "Content-Length", val_buf);
    htttp_set_body(&message, buf, buf_len);
    DTOR_INSERT(dtor, htttp_message_free, &message);

    unsigned char* message_buf;
    size_t message_buf_length;
    if (htttp_serialize(&message, &message_buf, &message_buf_length) == -1) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, message_buf);

    size_t encrypted_length;
    unsigned char* encrypted_msg = session_encrypt(*shared_key, message_buf, message_buf_length, &encrypted_length);
    if (encrypted_msg == NULL) {
        fprintf(stderr, "cannot encrypt message");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, encrypted_msg);

    if (send_uint32_t(fd, (uint32_t)encrypted_length) == -1 ||
        send_all(fd, encrypted_msg, encrypted_length) == -1) {
            perror("send_all");
            DTOR_RETURN(dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

int htttp_receive(int fd, htttp_message_t* message, unsigned char (*shared_key)[SESSION_KEY_LEN]) {
    DTOR_DEFINE(dtor, 10);

    uint32_t encrypted_length;
    if (recv_uint32_t(fd, &encrypted_length) == -1) {
        perror("recv");
        DTOR_RETURN(dtor, -1);
    }

    unsigned char encrypted_msg_static[1024];
    unsigned char* encrypted_msg = encrypted_msg_static;
    if (encrypted_length > sizeof(encrypted_msg_static)) {
        encrypted_msg = malloc(encrypted_length);

        if (encrypted_msg == NULL) {
            perror("malloc");
            DTOR_RETURN(dtor, -1);
        }
        DTOR_INSERT(dtor, free, encrypted_msg);
    }

    if (recv_all(fd, encrypted_msg, encrypted_length) == -1) {
        perror("recv_all");
        DTOR_RETURN(dtor, -1);
    }

    size_t length;
    unsigned char* msg = session_decrypt(*shared_key, encrypted_msg, encrypted_length, &length);
    if (msg == NULL) {
        fprintf(stderr, "cannot decrypt message");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, msg);

    htttp_message_init(message);
    if (htttp_parse(msg, length, message) == -1) {
        fprintf(stderr, "cannot parse message");
        DTOR_RETURN(dtor, -1);
    }
    
    DTOR_RETURN(dtor, 0);
}

static int handshake(int sockfd, unsigned char (*shared_key)[SESSION_KEY_LEN]) {
    DTOR_DEFINE(dtor, 10);

    unsigned char nonce_buf[SESSION_KEY_LEN];
    generate_session_key(nonce_buf);
    
    send_uint32_t(sockfd, SESSION_KEY_LEN);
    send_all(sockfd, nonce_buf, SESSION_KEY_LEN);
    
    uint32_t signed_nonce_len;
    recv_uint32_t(sockfd, &signed_nonce_len);

    unsigned char* signed_nonce_buf = read_bytes(sockfd, signed_nonce_len);
    DTOR_INSERT(dtor, free, signed_nonce_buf);

    uint32_t cert_len;
    recv_uint32_t(sockfd, &cert_len);

    unsigned char* cert_buf = read_bytes(sockfd, cert_len);
    DTOR_INSERT(dtor, free, cert_buf);

    X509* cert = load_cert_bytes(cert_buf, (int)cert_len);
    DTOR_INSERT(dtor, X509_free, cert);
    int verify_cert = verify_server_cert(cert, "auth/cacsertificate.crt");

    if (verify_cert == 0) {
        fprintf(stderr, "Unable to verify certificate.\n");
        DTOR_RETURN(dtor, -1);
    }

    int verify_message = verify_message_pss(cert, signed_nonce_buf, signed_nonce_len, nonce_buf, sizeof(nonce_buf));

    if (verify_message == 0) {
        fprintf(stderr, "Unable to verify signed nonce.\n");
        DTOR_RETURN(dtor, -1);
    }

    generate_session_key(*shared_key);

    EVP_PKEY* public_key = X509_get_pubkey(cert);
    DTOR_INSERT(dtor, EVP_PKEY_free, public_key);

    size_t encrypted_shared_key_len;
    unsigned char* encrypted_shared_key = rsa_encrypt_block(public_key, *shared_key, SESSION_KEY_LEN, &encrypted_shared_key_len, 1);
    if (encrypted_shared_key == NULL) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, free, encrypted_shared_key);

    send_uint32_t(sockfd, (uint32_t)encrypted_shared_key_len);
    send_all(sockfd, encrypted_shared_key, encrypted_shared_key_len);
    
    DTOR_RETURN(dtor, 0);
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 4321;
    const char *server_address = (argc > 2) ? argv[2] : "localhost";

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    struct hostent *he = gethostbyname(server_address);
    if (!he)
    {
        fprintf(stderr, "Cannot resolve host: %s\n", server_address);
        return 1;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);


    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    printf("connected to %s:%d\n", server_address, port);

    unsigned char shared_key[SESSION_KEY_LEN];
    if (handshake(sockfd, &shared_key) == -1) {
        perror("handshake");
        return 1;
    }

    while (true) {
        char line[MAX_MESSAGE_SIZE + 1] = {0};

        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        size_t line_len = strlen(line);

        if (line_len > 0 && line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
            line_len--;
        }

        if (line_len == 0) {
            continue;
        }

        if (line_len > MAX_MESSAGE_SIZE) {
            fprintf(stderr, "message too large\n");
            continue;
        }

        if (htttp_loop(sockfd, line, line_len, &shared_key) == -1) {
            break;
        }

        htttp_message_t reply;
        if (htttp_receive(sockfd, &reply, &shared_key) == -1) {
            break;
        }

        printf("Server response: %d %s %s\n", reply.status, reply.reason, reply.body);
        htttp_message_free(&reply);
    }

    close(sockfd);
    return 0;
}