#include "server.h"
#include "dtor.h"
#include "logger.h"
#include "timer.h"
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(Acceptor_free)
static DTOR_WRAPPER_DEFINE(Epoll_free)
static DTOR_WRAPPER_DEFINE(PlayerIo_free)
static DTOR_WRAPPER_DEFINE(AuthData_free)
static DTOR_WRAPPER_DEFINE(ServerApplication_free)

static void close_fd_ptr(int* fd) {
    close(*fd);
    *fd = -1;
}
static DTOR_WRAPPER_DEFINE(close_fd_ptr)

static void server_logger_free(bool* connected) {
    if (*connected) {
        (void)logger_free_ipc();
        *connected = false;
    }
}
static DTOR_WRAPPER_DEFINE(server_logger_free)

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == -1) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void server_logger_maintain(Server* server, uint64_t now_ms) {
    if (now_ms < server->logger_next_check_ms) {
        return;
    }
    server->logger_next_check_ms = now_ms +
        (uint64_t)server->cfg.logger_reconnect_seconds * 1000u;

    if (server->logger_connected &&
        LOGGER_LOG(LOG_DEBUG, "server", "logger health check") == -1) {
        server_logger_free(&server->logger_connected);
    }
    if (!server->logger_connected) {
        server->logger_connected = logger_init_ipc(server->cfg.log_ipc) == 0;
    }
}

int server_init(Server* server) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (config_var_init(&server->cfg) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, config_var_free, &server->cfg);

    server->logger_connected = logger_init_ipc(server->cfg.log_ipc) == 0;
    server->logger_next_check_ms = monotonic_ms() +
        (uint64_t)server->cfg.logger_reconnect_seconds * 1000u;
    if (server->logger_connected) {
        DTOR_INSERT(errdtor, server_logger_free, &server->logger_connected);
    }

    if (Acceptor_init(&server->acceptor, server->cfg.address, server->cfg.port, server->cfg.max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Acceptor_free, &server->acceptor);

    if (server->acceptor.listen_fd < 0 || (size_t)server->acceptor.listen_fd >= server->cfg.max_fds) {
        LOGGER_LOG(LOG_ERROR, "server", "listen fd out of table range");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    const int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        LOGGER_PERROR("server", "epoll_create1");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    if (Epoll_init(&server->epoll, server->cfg.max_fds, server->cfg.max_events, epoll_fd) == -1) {
        close(epoll_fd);
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Epoll_free, &server->epoll);

    server->state_timer_fd = periodic_timer_create(
        server->cfg.state_push_interval_ms
    );
    if (server->state_timer_fd == -1 ||
        (size_t)server->state_timer_fd >= server->cfg.max_fds) {
        if (server->state_timer_fd != -1) {
            close(server->state_timer_fd);
            server->state_timer_fd = -1;
        }
        LOGGER_LOG(LOG_ERROR, "server", "cannot create STATE timer");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, close_fd_ptr, &server->state_timer_fd);

    if (PlayerIo_init(&server->player_io, server->cfg.max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, PlayerIo_free, &server->player_io);

    if (AuthData_init(&server->auth, server->cfg.max_fds,
                      server->cfg.key_path, server->cfg.cert_path) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, AuthData_free, &server->auth);

    if (ServerApplication_init(&server->application, server->cfg.max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, ServerApplication_free, &server->application);

    if (Epoll_accept_one(&server->epoll, server->acceptor.listen_fd, EPOLL_ENTRY_ACCEPTOR, EPOLLIN) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    if (Epoll_accept_one(
        &server->epoll,
        server->state_timer_fd,
        EPOLL_ENTRY_ROOM_TIMERFD,
        EPOLLIN
    ) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    LOGGER_LOG(
        LOG_INFO,
        "server",
        "listening on %s:%d; STATE interval=%ums",
        server->cfg.address,
        server->cfg.port,
        server->cfg.state_push_interval_ms
    );

    DTOR_RETURN(dtor, 0);
}

void server_free(Server* server) {
    close_fd_ptr(&server->state_timer_fd);
    ServerApplication_free(&server->application);
    AuthData_free(&server->auth);
    PlayerIo_free(&server->player_io);
    Epoll_free(&server->epoll);
    Acceptor_free(&server->acceptor);
    config_var_free(&server->cfg);
    server_logger_free(&server->logger_connected);
}

void server_tick(Server* server) {
    bool acceptor_readable = false;
    bool state_timer_readable = false;
    if (Epoll_poll(&server->epoll, &server->player_io.players_reading,
                   &server->player_io.players_writing, &acceptor_readable,
                   &state_timer_readable) == -1) {
        return;
    }

    bool should_stop_accepting = false;
    if (acceptor_readable) {
        Acceptor_accept(&server->acceptor, server->cfg.max_fds, &server->acceptor.accepted, &should_stop_accepting);
        Epoll_accept(&server->epoll, &server->acceptor.accepted, EPOLL_ENTRY_PLAYER, EPOLLIN, &server->epoll.player_close_fds);
        PlayerIo_accept(&server->player_io, &server->acceptor.accepted, &server->epoll.player_close_fds,
                        server->cfg.client_capacity);
        AuthData_accept(&server->auth, &server->acceptor.accepted, &server->epoll.player_close_fds,
                        server->cfg.client_capacity);
    }

    PlayerIo_read(&server->player_io, &server->player_io.players_reading,
                  &server->player_io.read_qs, &server->epoll.player_close_fds);

    AuthData_handshake_or_decrypt(&server->auth, &server->player_io.read_qs, &server->auth.decrypt_qs,
                              &server->player_io.write_qs, &server->epoll.player_close_fds);

    ServerApplication_sync_authenticated(
        &server->application,
        &server->auth,
        &server->epoll.player_close_fds
    );
    ServerApplication_handle_requests(
        &server->application,
        &server->auth.decrypt_qs,
        &server->auth.auth_qs,
        &server->epoll.player_close_fds
    );
    if (state_timer_readable) {
        const uint64_t expirations = periodic_timer_drain(server->state_timer_fd);
        if (expirations != 0) {
            server_logger_maintain(server, monotonic_ms());
            ServerApplication_push_state(
                &server->application,
                &server->auth,
                &server->player_io,
                expirations,
                &server->auth.auth_qs,
                &server->epoll.player_close_fds
            );
        }
    }

    AuthData_encrypt(&server->auth, &server->auth.auth_qs, &server->player_io.write_qs,
                 &server->epoll.player_close_fds);

    PlayerIo_write(&server->player_io, &server->player_io.write_qs,
                   &server->player_io.players_writing, &server->epoll.player_close_fds,
                   &server->player_io.vec_write_qs_status);

    Epoll_sync_interest(&server->epoll, &server->player_io.vec_write_qs_status,
                        &server->epoll.player_close_fds,
                        server->acceptor.listen_fd, should_stop_accepting);

    ServerApplication_close(&server->application, &server->epoll.player_close_fds);
    PlayerIo_close(&server->player_io, &server->epoll.player_close_fds);
    AuthData_close(&server->auth, &server->epoll.player_close_fds);
    Epoll_close(&server->epoll, &server->epoll.player_close_fds);

    Acceptor_reset(&server->acceptor);
    PlayerIo_reset(&server->player_io);
    AuthData_reset(&server->auth);
    Epoll_reset(&server->epoll);
}
