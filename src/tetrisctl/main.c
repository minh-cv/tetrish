#include "cJSON.h"
#include "config_var.h"
#include "dtor.h"
#include "htttp.h"
#include "tetrissh.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    TETRISCTL_EXIT_OK = 0,
    TETRISCTL_EXIT_CONNECT = 2, // config or connection failure
    TETRISCTL_EXIT_IO = 3,      // request failed or non-2xx response
    TETRISCTL_EXIT_USAGE = 64,
};

typedef struct {
    const char* name;
    const char* method;
    const char* path;
} ControlCommand;

static const ControlCommand COMMANDS[] = {
    { "status", "GET", "/status" },
    { "shutdown", "POST", "/shutdown" },
    { "reload", "POST", "/reload" },
};

static void usage(void) {
    fputs("usage: tetrisctl <status|shutdown|reload> [--json]\n", stderr);
}

static const ControlCommand* command_find(const char* name) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

static void close_ptr(int* fd) {
    close(*fd);
}

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(config_var_free)

static int connect_control(const char* path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "control_ipc path too long: %s\n", path);
        return -1;
    }
    strcpy(addr.sun_path, path);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    // a wedged daemon must not hang the tool; EAGAIN from an expired timeout
    // surfaces as a frame I/O failure
    const struct timeval timeout = { .tv_sec = 3, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "cannot connect to %s: %s (is tetrisd running?)\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void print_body(const HtttpResponse* response, bool raw_json) {
    if (response->body_len == 0) {
        return;
    }
    if (!raw_json) {
        cJSON* json = cJSON_ParseWithLength((const char*)response->body, response->body_len);
        if (json != NULL) {
            char* pretty = cJSON_Print(json);
            cJSON_Delete(json);
            if (pretty != NULL) {
                puts(pretty);
                free(pretty);
                return;
            }
        }
        // non-JSON bodies fall through to raw printing
    }
    fwrite(response->body, 1, response->body_len, stdout);
    putchar('\n');
}

int main(int argc, char** argv) {
    DTOR_DEFINE(dtor, 10);

    const ControlCommand* cmd = NULL;
    bool raw_json = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            raw_json = true;
        }
        else if (cmd == NULL) {
            cmd = command_find(argv[i]);
            if (cmd == NULL) {
                usage();
                DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
            }
        }
        else {
            usage();
            DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
        }
    }
    if (cmd == NULL) {
        usage();
        DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
    }

    struct config_var cfg;
    if (config_var_init(&cfg) == -1) {
        DTOR_RETURN(dtor, TETRISCTL_EXIT_CONNECT);
    }
    DTOR_INSERT(dtor, config_var_free, &cfg);

    int fd = connect_control(cfg.control_ipc);
    if (fd == -1) {
        DTOR_RETURN(dtor, TETRISCTL_EXIT_CONNECT);
    }
    DTOR_INSERT(dtor, close_ptr, &fd);

    const HtttpMessage request = {
        .request = {
            .method = cmd->method,
            .path = cmd->path,
            .header = {{0}},
            .header_count = 0,
            .body = NULL,
            .body_len = 0,
        },
        .is_request = true,
    };

    size_t request_buf_len = 0;
    unsigned char* const request_buf = htttp_serialize(&request, &request_buf_len);
    if (request_buf == NULL || request_buf_len > FRAME_MAX) {
        free(request_buf);
        fputs("cannot serialize request\n", stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }
    DTOR_INSERT(dtor, free, request_buf);

    if (tetrish_send_frame(fd, request_buf, (uint32_t)request_buf_len, NULL) == -1) {
        fputs("cannot send request\n", stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }

    uint32_t reply_len = 0;
    unsigned char* const reply_buf = tetrish_recv_frame(fd, &reply_len, NULL);
    if (reply_buf == NULL) {
        // the daemon's busy policy is accept-and-close, which lands here too
        fputs("no response from tetrisd (busy or shutting down?)\n", stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }
    DTOR_INSERT(dtor, free, reply_buf);

    HtttpMessage reply;
    if (htttp_parse(reply_buf, reply_len, &reply) == -1 || reply.is_request) {
        fputs("malformed response from tetrisd\n", stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }

    print_body(&reply.response, raw_json);

    if (reply.response.status < 200 || reply.response.status >= 300) {
        fprintf(stderr, "tetrisd responded %d %s\n",
                (int)reply.response.status, reply.response.reason);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }
    DTOR_RETURN(dtor, TETRISCTL_EXIT_OK);
}
