/*
    The admin CLI. One shot: connect to the daemon's Unix control socket, send
    one HTTTP request, print the response, exit. No epoll, no handshake, no
    daemonizing — the channel is local and the exchange is a single round trip,
    so anything more would be machinery without a purpose.

    Exit codes are the interface for shell scripting and for tetrish builtins:
    0 for a 2xx, 1 for a usage error, 2 for "cannot reach the daemon", 3 for a
    4xx, 4 for a 5xx.
*/

#include "config_var.h"
#include "dtor.h"
#include "htttp.h"
#include "tetrissh.h"
#include "wire.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define EXIT_USAGE 1
#define EXIT_UNREACHABLE 2
#define EXIT_CLIENT_ERROR 3
#define EXIT_SERVER_ERROR 4

typedef struct {
    const char* name;
    const char* method;
    const char* path;
    const char* help;
} Command;

static const Command COMMANDS[] = {
    {"status", "GET", "/ctl/status", "print a snapshot of the running daemon"},
    {"shutdown", "POST", "/ctl/shutdown", "ask the daemon to exit gracefully"},
    {"drain", "POST", "/ctl/drain", "stop admitting new players"},
};

static const size_t COMMAND_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(free)

static void usage(void) {
    fprintf(stderr, "usage: tetrisctl [--socket PATH] [--timeout MS] <command>\n\ncommands:\n");
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        fprintf(stderr, "  %-10s %s\n", COMMANDS[i].name, COMMANDS[i].help);
    }
}

static int connect_control(const char* path, unsigned timeout_ms) {
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "control socket path too long: %s\n", path);
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return -1;
    }

    // so a wedged daemon cannot hang the CLI; "daemon not running" and
    // "daemon not answering" are the same outcome to the caller
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "cannot reach tetrisd at %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_command(int fd, const Command* command) {
    HtttpMessage message;
    memset(&message, 0, sizeof(message));
    message.is_request = true;
    message.request.method = command->method;
    message.request.path = command->path;

    size_t length;
    unsigned char* const buffer = htttp_serialize(&message, &length);
    if (buffer == NULL || length == 0 || length > FRAME_MAX) {
        free(buffer);
        fprintf(stderr, "cannot serialize the request\n");
        return -1;
    }

    const int rc = tetrish_send_frame(fd, buffer, (uint32_t)length, NULL);
    free(buffer);
    return rc;
}

/*
    Prints the body and turns the status into an exit code. The body is
    line-oriented `key=value` text, which is what makes `tetrisctl status |
    grep` a reasonable thing for a shell script to do.
*/
static int print_response(int fd) {
    DTOR_DEFINE(dtor, 3);

    uint32_t length;
    unsigned char* const frame = tetrish_recv_frame(fd, &length, NULL);
    if (frame == NULL) {
        fprintf(stderr, "no answer from tetrisd\n");
        DTOR_RETURN(dtor, EXIT_UNREACHABLE);
    }
    DTOR_INSERT(dtor, free, frame);

    HtttpMessage message;
    if (htttp_parse(frame, length, &message) == -1 || message.is_request) {
        fprintf(stderr, "tetrisd sent something unreadable\n");
        DTOR_RETURN(dtor, EXIT_SERVER_ERROR);
    }

    if (message.response.body_len != 0) {
        fwrite(message.response.body, 1, message.response.body_len, stdout);
    }

    const int status = (int)message.response.status;
    if (status >= 200 && status < 300) {
        DTOR_RETURN(dtor, 0);
    }
    fprintf(stderr, "%d %s\n", status, message.response.reason);
    DTOR_RETURN(dtor, status < 500 ? EXIT_CLIENT_ERROR : EXIT_SERVER_ERROR);
}

int main(int argc, char** argv) {
    DTOR_DEFINE(dtor, 5);

    const char* socket_override = NULL;
    unsigned timeout_ms = 3000;
    const char* name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_override = argv[++i];
        }
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            const long value = strtol(argv[++i], NULL, 10);
            if (value <= 0) {
                usage();
                DTOR_RETURN(dtor, EXIT_USAGE);
            }
            timeout_ms = (unsigned)value;
        }
        else if (name == NULL && argv[i][0] != '-') {
            name = argv[i];
        }
        else {
            usage();
            DTOR_RETURN(dtor, EXIT_USAGE);
        }
    }

    if (name == NULL) {
        usage();
        DTOR_RETURN(dtor, EXIT_USAGE);
    }

    const Command* command = NULL;
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(name, COMMANDS[i].name) == 0) {
            command = &COMMANDS[i];
            break;
        }
    }
    if (command == NULL) {
        fprintf(stderr, "unknown command: %s\n", name);
        usage();
        DTOR_RETURN(dtor, EXIT_USAGE);
    }

    const char* path = socket_override;
    struct config_var cfg;
    if (path == NULL) {
        if (config_var_init(&cfg) == -1) {
            DTOR_RETURN(dtor, EXIT_USAGE);
        }
        DTOR_INSERT(dtor, config_var_free, &cfg);
        path = cfg.ctl_ipc;
    }

    const int fd = connect_control(path, timeout_ms);
    if (fd == -1) {
        DTOR_RETURN(dtor, EXIT_UNREACHABLE);
    }

    if (send_command(fd, command) == -1) {
        close(fd);
        DTOR_RETURN(dtor, EXIT_UNREACHABLE);
    }

    const int code = print_response(fd);
    close(fd);
    DTOR_RETURN(dtor, code);
}
