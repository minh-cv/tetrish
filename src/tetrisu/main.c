#include "common.h"
#include "config_var.h"
#include "htttp.h"
#include <arpa/inet.h>
#include <assert.h>
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

DTOR_WRAPPER_DEFINE(config_var_free)

static void close_ptr(int* fd) {
    close(*fd);
}

DTOR_WRAPPER_DEFINE(close_ptr)
DTOR_WRAPPER_DEFINE(freeaddrinfo)

int prepare_socket(int port, const char* address) {
    DTOR_DEFINE(dtor, 10);
    
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);    

    struct addrinfo* res;
    int rc = getaddrinfo(address, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, freeaddrinfo, res);

    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        int listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

        if (listen_fd == -1) {
            perror("socket");
            continue;
        }

        if (connect(listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("connect");
            close(listen_fd);
            continue;
        }

        DTOR_RETURN(dtor, listen_fd);

    }

    fprintf(stderr, "cannot connect\n");
    DTOR_RETURN(dtor, -1);
}


int main() {
    DTOR_DEFINE(dtor, 10);

    struct config_var cfg;
    if (config_var_init(&cfg) == -1) {
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, config_var_free, &cfg);

    int port = cfg.port;
    const char* server_address = cfg.address;

    int sockfd = prepare_socket(port, server_address);
    if (sockfd == -1) {
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, close_ptr, &sockfd);

    SessionKey shared_key;
    if (tetrish_client_handshake(sockfd, cfg.ca_path, &shared_key) == -1) {
        perror("handshake");
        DTOR_RETURN(dtor, 1);
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

    DTOR_RETURN(dtor, 0);
}