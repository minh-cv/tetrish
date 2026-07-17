#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "config_var.h"
#include "dtor.h"
#include "tetrissh.h"
#include <fcntl.h>

volatile sig_atomic_t running = 1;
volatile sig_atomic_t flush = 0;

static void sigterm_handler(int _) {
    (void)_;
    running = 0;
}

static void sighup_handler(int _) {
    (void)_;
    flush = 1;
}

static void close_ptr(int* ptr) {
    close(*ptr);
}

static void unlink_cfg_log_ipc(struct config_var* cfg) {
    if (unlink(cfg->log_ipc) == -1 && errno != ENOENT) {
        perror("unlink");
    }
}

static void fclose_ptr(FILE** file) {
    fclose(*file);
}

static DTOR_WRAPPER_DEFINE(fclose_ptr)
static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(unlink_cfg_log_ipc)

static int prepare_socket(const char* path) {
    DTOR_DEFINE(dtor, 10);

    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "log_ipc path too long: %s\n", path);
        DTOR_RETURN(dtor, -1);
    }

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, close_ptr, &listen_fd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (unlink(path) == -1 && errno != ENOENT) {
        perror("unlink");
        DTOR_RETURN(dtor, -1);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        DTOR_RETURN(dtor, -1);
    }

    if (listen(listen_fd, 1) == -1) {
        perror("listen");
        DTOR_RETURN(dtor, -1);
    }

    return listen_fd;
}

int main() {
    DTOR_DEFINE(dtor, 10);

    struct sigaction sigterm_act = {0};
    sigterm_act.sa_handler = sigterm_handler;
    sigemptyset(&sigterm_act.sa_mask);
    sigaction(SIGTERM, &sigterm_act, NULL);

    struct sigaction sighup_act = {0};
    sighup_act.sa_handler = sighup_handler;
    sigemptyset(&sighup_act.sa_mask);
    sigaction(SIGHUP, &sighup_act, NULL);

    struct config_var cfg;
    if (config_var_init(&cfg) == -1) {
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, config_var_free, &cfg);

    FILE* out_file = fopen(cfg.log_path, "a");
    if (out_file == NULL) {
        perror("out_file");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, fclose_ptr, &out_file);

    int fd = prepare_socket(cfg.log_ipc);
    if (fd == -1) {
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, close_ptr, &fd);
    DTOR_INSERT(dtor, unlink_cfg_log_ipc, &cfg);

    bool is_connected = false;
    int server_fd;

    while (running) {
        if (flush) {
            flush = 0;

            // Acquire and validate every new resource before tearing down the
            // old ones, so a bad reload (unparsable .tetrishrc, unwritable
            // log_path, unbindable log_ipc) leaves the running daemon untouched.
            struct config_var new_cfg;
            if (config_var_init(&new_cfg) == -1) {
                fprintf(stderr, "Reconfiguration failed\n");
                continue;
            }

            FILE* new_out_file = fopen(new_cfg.log_path, "a");
            if (new_out_file == NULL) {
                perror("fopen");
                config_var_free(&new_cfg);
                continue;
            }

            int new_fd = prepare_socket(new_cfg.log_ipc);
            if (new_fd == -1) {
                fclose(new_out_file);
                config_var_free(&new_cfg);
                continue;
            }

            if (fflush(out_file) == -1) {
                perror("fflush");
            }
            if (fclose(out_file) == -1) {
                perror("fclose");
            }
            out_file = new_out_file;

            close(fd);
            if (strcmp(cfg.log_ipc, new_cfg.log_ipc) != 0 &&
                unlink(cfg.log_ipc) == -1 && errno != ENOENT) {
                perror("unlink");
            }
            fd = new_fd;

            config_var_free(&cfg);
            cfg = new_cfg;

            puts("Reconfigured\n");
            continue;
        }

        if (!is_connected) {
            if ((server_fd = accept(fd, NULL, NULL)) == -1) {
                if (errno == EINTR) {
                    continue;
                }
                perror("accept");
                continue;
            }
            is_connected = true;
            continue;
        }

        // Gate the blocking tetrish_recv_frame() call behind poll(): its internal
        // recv loop retries silently on EINTR, so calling it directly on an idle
        // connection would swallow SIGTERM/SIGHUP until more data arrives. Poll
        // with a timeout keeps `running`/`flush` checked regularly instead.
        struct pollfd pfd = { .fd = server_fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 1000);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            continue;
        }
        if (ready == 0) {
            continue;
        }

        uint32_t frame_length;
        unsigned char* frame = tetrish_recv_frame(server_fd, &frame_length, NULL);
        if (frame == NULL) {
            fprintf(stderr, "tetrish_recv_frame: connection closed or invalid frame\n");
            is_connected = false;
            close(server_fd);
            continue;
        }

        // do whatever with the frame, for now just log directly
        if (fwrite(frame, frame_length, 1, out_file) != 1) {
            perror("fwrite");
            free(frame);
            DTOR_RETURN(dtor, 1);
        }

        if (fflush(out_file) == -1) {
            perror("fflush");
            free(frame);
            DTOR_RETURN(dtor, 1);
        }

        free(frame);
        continue;
    }

    if (fflush(out_file) == -1) {
        perror("fflush");
        DTOR_RETURN(dtor, 1);
    }

    if (is_connected) {
        close(server_fd);
    }

    DTOR_RETURN(dtor, 0);
}
