#include "server.h"
#include "dtor.h"
#include "htttp_layer.h"
#include "logger.h"
#include <sys/epoll.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(Acceptor_free)
static DTOR_WRAPPER_DEFINE(Epoll_free)
static DTOR_WRAPPER_DEFINE(PlayerIo_free)
static DTOR_WRAPPER_DEFINE(AuthData_free)
static DTOR_WRAPPER_DEFINE(HtttpData_free)
static DTOR_WRAPPER_DEFINE(AppData_free)


int server_init(Server* server) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (config_var_init(&server->cfg) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, config_var_free, &server->cfg);

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

    if (PlayerIo_init(&server->player_io, server->cfg.max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, PlayerIo_free, &server->player_io);

    if (AuthData_init(&server->auth, server->cfg.max_fds,
                      server->cfg.key_path, server->cfg.cert_path) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, AuthData_free, &server->auth);

    if (HtttpData_init(&server->htttp, server->cfg.max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, HtttpData_free, &server->htttp);

    if (AppData_init(&server->app, server->cfg.max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, AppData_free, &server->app);

    if (Epoll_accept_one(&server->epoll, server->acceptor.listen_fd, EPOLL_ENTRY_ACCEPTOR, EPOLLIN) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

void server_free(Server* server) {
    AppData_free(&server->app);
    HtttpData_free(&server->htttp);
    AuthData_free(&server->auth);
    PlayerIo_free(&server->player_io);
    Epoll_free(&server->epoll);
    Acceptor_free(&server->acceptor);
    config_var_free(&server->cfg);
}

void server_tick(Server* server) {
    bool acceptor_readable = false;
    if (Epoll_poll(&server->epoll, &server->player_io.players_reading,
                   &server->player_io.players_writing, &acceptor_readable) == -1) {
        return;
    }

    bool should_stop_accepting = false;
    if (acceptor_readable) {
        Acceptor_accept(&server->acceptor, server->cfg.max_fds, &server->acceptor.accepted, &should_stop_accepting);
        Epoll_accept(&server->epoll, &server->acceptor.accepted, EPOLL_ENTRY_PLAYER, EPOLLIN, &server->epoll.player_close_fds);
        // capacity contract: every layer's queues are accepted with the same
        // cfg.client_capacity and each stage emits at most one frame per
        // input frame, so no chained queue can overflow; the stages state
        // this as a precondition and fail the fd on violation
        PlayerIo_accept(&server->player_io, &server->acceptor.accepted, &server->epoll.player_close_fds,
                        server->cfg.client_capacity);
        AuthData_accept(&server->auth, &server->acceptor.accepted, &server->epoll.player_close_fds,
                        server->cfg.client_capacity);
        HtttpData_accept(&server->htttp, &server->acceptor.accepted, &server->epoll.player_close_fds,
                         server->cfg.client_capacity);
AppData_accept(&server->app, &server->acceptor.accepted, &server->epoll.player_close_fds);
    }

    PlayerIo_read(&server->player_io, &server->player_io.players_reading,
                  &server->player_io.read_qs, &server->epoll.player_close_fds);

    AuthData_handshake_or_decrypt(&server->auth, &server->player_io.read_qs, &server->auth.decrypt_qs,
                              &server->player_io.write_qs, &server->epoll.player_close_fds);

    HtttpData_parse(&server->htttp, &server->auth.decrypt_qs, &server->htttp.parsed_qs,
                    &server->epoll.player_close_fds);

    // TODO: dummy application layer — replace the echo with real game logic
    AppData_respond(&server->app, &server->htttp.parsed_qs, &server->htttp.response_qs,
                                  &server->epoll.player_close_fds);

    HtttpData_serialize(&server->htttp, &server->htttp.response_qs, &server->auth.encrypt_qs,
                        &server->epoll.player_close_fds);

    AuthData_encrypt(&server->auth, &server->auth.encrypt_qs, &server->player_io.write_qs,
                 &server->epoll.player_close_fds);

    PlayerIo_write(&server->player_io, &server->player_io.write_qs,
                   &server->player_io.players_writing, &server->epoll.player_close_fds,
                   &server->player_io.vec_write_qs_status);

    Epoll_sync_interest(&server->epoll, &server->player_io.vec_write_qs_status,
                        &server->epoll.player_close_fds,
                        server->acceptor.listen_fd, should_stop_accepting);

    PlayerIo_close(&server->player_io, &server->epoll.player_close_fds);
    AuthData_close(&server->auth, &server->epoll.player_close_fds);
    HtttpData_close(&server->htttp, &server->epoll.player_close_fds);
AppData_close(&server->app, &server->epoll.player_close_fds);
    Epoll_close(&server->epoll, &server->epoll.player_close_fds);

    Acceptor_reset(&server->acceptor);
    PlayerIo_reset(&server->player_io);
    AuthData_reset(&server->auth);
    HtttpData_reset(&server->htttp);
    Epoll_reset(&server->epoll);
}
