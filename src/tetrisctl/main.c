#include "cJSON.h"
#include "config_var.h"
#include "dtor.h"
#include "htttp.h"
#include "tetrissh.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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
    TETRISCTL_EXIT_CONFIG = 2,      // PROJECT_DIR or .tetrishrc unusable
    TETRISCTL_EXIT_UNREACHABLE = 3, // cannot connect to the control socket
    TETRISCTL_EXIT_IO = 4,          // request failed, or the reply was unreadable
    TETRISCTL_EXIT_STATUS = 5,      // the daemon answered, with a non-2xx
    TETRISCTL_EXIT_USAGE = 64,
};

#define DEFAULT_TIMEOUT_MS 3000

typedef struct {
    const char* name;
    const char* method;
    const char* path;
    const char* help;
} ControlCommand;

static const ControlCommand COMMANDS[] = {
    { "status", "GET", "/status", "print a snapshot of the running daemon" },
    { "shutdown", "POST", "/shutdown", "ask the daemon to exit once the reply is out" },
    { "reload", "POST", "/reload", "re-read the reloadable directives" },
};

static void usage(FILE* out) {
    fputs("usage: tetrisctl [--socket PATH] [--timeout MS] [--raw] <command>\n\ncommands:\n", out);
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        fprintf(out, "  %-9s %s\n", COMMANDS[i].name, COMMANDS[i].help);
    }
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

/*
    connect(2) is not covered by SO_SNDTIMEO, so it is issued non-blocking and
    bounded by poll: a daemon that is stopped or wedged before its accept loop
    leaves the backlog full, and a blocking connect would hang there forever.
    The socket goes back to blocking afterwards, which is what the framing
    helpers and the SO_*TIMEO deadlines expect.
*/
static int connect_deadlined(int fd, const struct sockaddr_un* addr, unsigned timeout_ms) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        return -1;
    }

    if (connect(fd, (const struct sockaddr*)addr, sizeof(*addr)) == -1) {
        if (errno != EINPROGRESS) {
            return -1;
        }
        struct pollfd pfd = { fd, POLLOUT, 0 };
        const int ready = poll(&pfd, 1, (int)timeout_ms);
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (ready == -1) {
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
            return -1;
        }
        if (err != 0) {
            errno = err;
            return -1;
        }
    }

    if (fcntl(fd, F_SETFL, flags) == -1) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

static int connect_control(const char* path, unsigned timeout_ms) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "control socket path too long: %s\n", path);
        return -1;
    }
    strcpy(addr.sun_path, path);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    const struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    if (connect_deadlined(fd, &addr, timeout_ms) == -1) {
        fprintf(stderr, "cannot connect to %s: %s (is tetrisd running?)\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/*
    Raw output is byte-exact, so `tetrisctl status --raw` can be piped to a
    hash or a parser and match what the daemon sent; the pretty path is the
    human one and adds its own newline.
*/
static void print_body(const HtttpResponse* response, bool raw) {
    if (response->body_len == 0) {
        return;
    }
    if (!raw) {
        /*
            The body is the tail of the frame and carries no NUL, so
            require_null_terminated cannot be used to reject trailing garbage.
            The parse end is compared against the framed length instead, which
            catches the same thing without needing a terminator.
        */
        const char* const body = (const char*)response->body;
        const char* parse_end = NULL;
        cJSON* json = cJSON_ParseWithLengthOpts(body, response->body_len, &parse_end, 0);
        if (json != NULL) {
            while (parse_end < body + response->body_len && isspace((unsigned char)*parse_end)) {
                parse_end++;
            }
            char* pretty = parse_end == body + response->body_len ? cJSON_Print(json) : NULL;
            cJSON_Delete(json);
            if (pretty != NULL) {
                puts(pretty);
                free(pretty);
                return;
            }
        }
        // non-JSON and trailing-garbage bodies fall through to raw printing
    }
    fwrite(response->body, 1, response->body_len, stdout);
}

int main(int argc, char** argv) {
    DTOR_DEFINE(dtor, 10);

    // the daemon's busy policy is accept-and-close, so a send can land on a
    // peer that has already gone: without this the process dies on SIGPIPE
    // before it can report anything
    signal(SIGPIPE, SIG_IGN);

    const ControlCommand* cmd = NULL;
    const char* socket_override = NULL;
    unsigned timeout_ms = DEFAULT_TIMEOUT_MS;
    bool raw = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            DTOR_RETURN(dtor, TETRISCTL_EXIT_OK);
        }
        else if (strcmp(argv[i], "--raw") == 0) {
            raw = true;
        }
        else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_override = argv[++i];
        }
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            const long value = strtol(argv[++i], NULL, 10);
            if (value <= 0 || value > INT32_MAX) {
                usage(stderr);
                DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
            }
            timeout_ms = (unsigned)value;
        }
        else if (cmd == NULL && argv[i][0] != '-') {
            cmd = command_find(argv[i]);
            if (cmd == NULL) {
                fprintf(stderr, "unknown command: %s\n", argv[i]);
                usage(stderr);
                DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
            }
        }
        else {
            usage(stderr);
            DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
        }
    }
    if (cmd == NULL) {
        usage(stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_USAGE);
    }

    const char* path = socket_override;
    struct config_var cfg;
    if (path == NULL) {
        if (config_var_init(&cfg) == -1) {
            DTOR_RETURN(dtor, TETRISCTL_EXIT_CONFIG);
        }
        DTOR_INSERT(dtor, config_var_free, &cfg);
        path = cfg.control_ipc;
    }

    int fd = connect_control(path, timeout_ms);
    if (fd == -1) {
        DTOR_RETURN(dtor, TETRISCTL_EXIT_UNREACHABLE);
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
        // the busy policy closes without answering, and so lands here
        fputs("no response from tetrisd (busy or shutting down?)\n", stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }
    DTOR_INSERT(dtor, free, reply_buf);

    HtttpMessage reply;
    if (htttp_parse(reply_buf, reply_len, &reply) == -1 || reply.is_request) {
        fputs("malformed response from tetrisd\n", stderr);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_IO);
    }

    print_body(&reply.response, raw);

    if (reply.response.status < 200 || reply.response.status >= 300) {
        fprintf(stderr, "tetrisd responded %d %s\n",
                (int)reply.response.status, reply.response.reason);
        DTOR_RETURN(dtor, TETRISCTL_EXIT_STATUS);
    }
    DTOR_RETURN(dtor, TETRISCTL_EXIT_OK);
}
