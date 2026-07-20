#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "config_var.h"
#ifndef TETRISH_TETRISLOGD_NO_DAEMON
#include "daemon.h"
#endif
#include "dtor.h"
#include "tetrissh.h"
#include <fcntl.h>

volatile sig_atomic_t running = 1;
volatile sig_atomic_t flush = 0;

typedef struct {
    pthread_mutex_t out_file_mu;
    FILE* out_file;

    pthread_mutex_t clients_mu;
    int client_count;
    int max_clients;
} LogdCtx;

typedef struct {
    int fd;
    LogdCtx* ctx;
} ClientArg;

static void* client_thread_main(void* arg_v) {
    ClientArg* arg = arg_v;
    int fd = arg->fd;
    LogdCtx* ctx = arg->ctx;
    free(arg);

    while (running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 1000);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (ready == 0) {
            continue;
        }

        uint32_t frame_length;
        unsigned char* frame = tetrish_recv_frame(fd, &frame_length, NULL);
        if (frame == NULL) {
            break;
        }

        pthread_mutex_lock(&ctx->out_file_mu);
        bool write_ok = fwrite(frame, frame_length, 1, ctx->out_file) == 1 &&
                         fflush(ctx->out_file) != EOF;
        pthread_mutex_unlock(&ctx->out_file_mu);
        free(frame);
        if (!write_ok) {
            perror("fwrite/fflush");
            break;
        }
    }

    close(fd);
    pthread_mutex_lock(&ctx->clients_mu);
    ctx->client_count--;
    pthread_mutex_unlock(&ctx->clients_mu);
    return NULL;
}

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

static void mutex_destroy_ptr(pthread_mutex_t* mutex) {
    pthread_mutex_destroy(mutex);
}

static DTOR_WRAPPER_DEFINE(fclose_ptr)
static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(unlink_cfg_log_ipc)
static DTOR_WRAPPER_DEFINE(mutex_destroy_ptr)

static int prepare_socket(const char* path, int backlog) {
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

    if (listen(listen_fd, backlog) == -1) {
        perror("listen");
        DTOR_RETURN(dtor, -1);
    }

    return listen_fd;
}

int main() {
    DTOR_DEFINE(dtor, 10);

    #ifndef TETRISH_TETRISLOGD_NO_DAEMON
    switch (incantation()) {
    case 0:
        DTOR_RETURN(dtor, 0);
    case -1:
        perror("incantation");
        DTOR_RETURN(dtor, 1);
    case 1:
        break;
    default:
        assert(false);
    }
    #endif

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

    LogdCtx ctx = {0};
    pthread_mutex_init(&ctx.out_file_mu, NULL);
    DTOR_INSERT(dtor, mutex_destroy_ptr, &ctx.out_file_mu);
    pthread_mutex_init(&ctx.clients_mu, NULL);
    DTOR_INSERT(dtor, mutex_destroy_ptr, &ctx.clients_mu);
    ctx.max_clients = cfg.max_clients;

    FILE* out_file = fopen(cfg.log_path, "a");
    if (out_file == NULL) {
        perror("out_file");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, fclose_ptr, &out_file);
    ctx.out_file = out_file;

    int fd = prepare_socket(cfg.log_ipc, cfg.max_clients);
    if (fd == -1) {
        DTOR_RETURN(dtor, 1);
    }
    DTOR_INSERT(dtor, close_ptr, &fd);
    DTOR_INSERT(dtor, unlink_cfg_log_ipc, &cfg);

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

            int new_fd = prepare_socket(new_cfg.log_ipc, new_cfg.max_clients);
            if (new_fd == -1) {
                fclose(new_out_file);
                config_var_free(&new_cfg);
                continue;
            }

            pthread_mutex_lock(&ctx.out_file_mu);
            if (fflush(out_file) == -1) {
                perror("fflush");
            }
            if (fclose(out_file) == -1) {
                perror("fclose");
            }
            out_file = new_out_file;
            ctx.out_file = new_out_file;
            pthread_mutex_unlock(&ctx.out_file_mu);

            close(fd);
            if (strcmp(cfg.log_ipc, new_cfg.log_ipc) != 0 &&
                unlink(cfg.log_ipc) == -1 && errno != ENOENT) {
                perror("unlink");
            }
            fd = new_fd;

            pthread_mutex_lock(&ctx.clients_mu);
            ctx.max_clients = new_cfg.max_clients;
            pthread_mutex_unlock(&ctx.clients_mu);

            config_var_free(&cfg);
            cfg = new_cfg;

            puts("Reconfigured\n");
            continue;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
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

        int client_fd = accept(fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&ctx.clients_mu);
        bool full = ctx.client_count >= ctx.max_clients;
        if (!full) {
            ctx.client_count++;
        }
        pthread_mutex_unlock(&ctx.clients_mu);

        if (full) {
            fprintf(stderr, "logd_max_clients reached, rejecting connection\n");
            close(client_fd);
            continue;
        }

        ClientArg* arg = malloc(sizeof(*arg));
        if (arg == NULL) {
            perror("malloc");
            close(client_fd);
            pthread_mutex_lock(&ctx.clients_mu);
            ctx.client_count--;
            pthread_mutex_unlock(&ctx.clients_mu);
            continue;
        }
        arg->fd = client_fd;
        arg->ctx = &ctx;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread_main, arg) != 0) {
            perror("pthread_create");
            free(arg);
            close(client_fd);
            pthread_mutex_lock(&ctx.clients_mu);
            ctx.client_count--;
            pthread_mutex_unlock(&ctx.clients_mu);
            continue;
        }
        pthread_detach(tid);
    }

    struct timespec drain_ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
    for (;;) {
        pthread_mutex_lock(&ctx.clients_mu);
        int remaining = ctx.client_count;
        pthread_mutex_unlock(&ctx.clients_mu);
        if (remaining == 0) {
            break;
        }
        nanosleep(&drain_ts, NULL);
    }

    if (fflush(out_file) == -1) {
        perror("fflush");
        DTOR_RETURN(dtor, 1);
    }

    DTOR_RETURN(dtor, 0);
}
