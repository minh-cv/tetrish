#include "htttp.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <dtor.h>

#define SERVER_PORT 12345
#define SERVER_IP "127.0.0.1"
#define MAX_MESSAGE_SIZE 4096

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

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
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

static DTOR_WRAPPER_DEFINE(htttp_message_free)
static DTOR_WRAPPER_DEFINE(free)

int htttp_loop(int fd, const char* buf, size_t buf_len) {
    DTOR_DEFINE(dtor, 10);
    
    htttp_message_t message;
    htttp_message_init(&message);
    message.kind = HTTTP_REQUEST;
    strncpy(message.path, "/insert/path/here", HTTTP_MAX_PATH);

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

    uint8_t message_length_buf[4];
    encode_u32_be(message_length_buf, (uint32_t)message_buf_length);

    if (send_all(fd, message_length_buf, sizeof(message_length_buf)) == -1 ||
        send_all(fd, message_buf, message_buf_length) == -1) {
            perror("send_all");
            DTOR_RETURN(dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

int htttp_receive(int fd, htttp_message_t* message) {
    uint8_t recv_buf[4];
    if (recv_all(fd, recv_buf, sizeof(recv_buf)) == -1) {
        perror("recv_all");
        return -1;
    }

    uint32_t length = decode_u32_be(recv_buf);
    unsigned char msg_static[1024];
    unsigned char* msg = msg_static;
    if (length > sizeof(msg_static)) {
        msg = malloc(length);

        if (msg == NULL) {
            perror("malloc");
            return -1;
        }
    }

    if (recv_all(fd, msg, length) == -1) {
        perror("recv_all");
        return -1;
    }

    htttp_message_init(message);
    if (htttp_parse(msg, length, message) == -1) {
        perror("cannot parse message");
        return -1;
    }

    if (msg != msg_static) {
        free(msg);
    }

    return 0;
}

int main(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) != 1) {
        perror("inet_pton");
        close(sockfd);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    printf("connected to %s:%d\n", SERVER_IP, SERVER_PORT);

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

        if (htttp_loop(sockfd, line, line_len) == -1) {
            break;
        }

        htttp_message_t reply;
        if (htttp_receive(sockfd, &reply) == -1) {
            break;
        }

        printf("%d %s %s", reply.status, reply.reason, reply.body);
        htttp_message_free(&reply);

        // uint8_t len_buf[4];
        // if (recv_all(sockfd, len_buf, 4) == -1) {
        //     perror("recv_all length");
        //     break;
        // }

        // uint32_t length = decode_u32_be(len_buf);
        // unsigned char msg_static[1024];
        // unsigned char* msg = msg_static;
        // if (length >= sizeof(msg_static)) {
        //     msg = malloc(length);

        //     if (msg == NULL) {
        //         perror("malloc");
        //         return -1;
        //     }
        // }

        // if (recv_all(sockfd, msg, length) == -1) {
        //     perror("recv_all");
        //     return -1;
        // }

        // msg[length] = '\0';
        // printf("%s", msg);

        // if (msg != msg_static) {
        //     free(msg);
        // }
    }

    close(sockfd);
    return 0;
}