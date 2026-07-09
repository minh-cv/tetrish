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
#include "tetrissh.h"

#include <dtor.h>

#define MAX_MESSAGE_SIZE 4096

static DTOR_WRAPPER_DEFINE(htttp_message_free)
static DTOR_WRAPPER_DEFINE(free)

int htttp_loop(int fd, const char* buf, size_t buf_len, SessionKey* shared_key) {
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

    if (message_buf_length > UINT32_MAX) {
        DTOR_RETURN(dtor, -1);
    }

    if (tetrish_send_frame(fd, message_buf, (uint32_t)message_buf_length, shared_key) == -1) {
        DTOR_RETURN(dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

int htttp_receive(int fd, htttp_message_t* message, unsigned char (*shared_key)[SESSION_KEY_LEN]) {
    DTOR_DEFINE(dtor, 10);
    uint32_t length;
    unsigned char* msg = tetrish_recv_frame(fd, &length, shared_key);
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

    SessionKey shared_key;
    if (tetrish_client_handshake(sockfd, "auth/cacsertificate.crt", &shared_key) == -1) {
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