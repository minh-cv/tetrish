#include "server.h"
#include "dtor.h"
#include "htttp_layer.h"
#include "logger.h"
#include "sig.h"
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(Acceptor_free)
static DTOR_WRAPPER_DEFINE(Epoll_free)
static DTOR_WRAPPER_DEFINE(PlayerIo_free)
static DTOR_WRAPPER_DEFINE(AuthData_free)
static DTOR_WRAPPER_DEFINE(HtttpData_free)
static DTOR_WRAPPER_DEFINE(AppData_free)

static DTOR_WRAPPER_DEFINE(Control_free)

static void clamp_max_player_fd(struct config_var* cfg) {
    if (cfg->max_player_fd > cfg->max_fds) {
        LOGGER_LOG(LOG_WARN, "server", "max_player_fd %u exceeds max_fds %u, clamping",
                   cfg->max_player_fd, cfg->max_fds);
        cfg->max_player_fd = cfg->max_fds;
    }
}

int server_init(Server* server) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    if (config_var_init(&server->cfg) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, config_var_free, &server->cfg);

    struct rlimit nofile;
    if (getrlimit(RLIMIT_NOFILE, &nofile) == -1) {
        LOGGER_PERROR("server", "getrlimit");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    if (nofile.rlim_cur != RLIM_INFINITY && (rlim_t)server->cfg.max_fds > nofile.rlim_cur) {
        LOGGER_LOG(LOG_WARN, "server", "max_fds %u exceeds RLIMIT_NOFILE %llu, clamping",
                   server->cfg.max_fds, (unsigned long long)nofile.rlim_cur);
        server->cfg.max_fds = (unsigned int)nofile.rlim_cur;
    }
    clamp_max_player_fd(&server->cfg);
    const size_t epoll_capacity = (size_t)server->cfg.max_fds;

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
    if (Epoll_init(&server->epoll, epoll_capacity, server->cfg.max_events, epoll_fd) == -1) {
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

    if (AppData_init(&server->app, server->cfg.max_fds, server->cfg.max_rooms) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, AppData_free, &server->app);
    if (Control_init(&server->control, server->cfg.control_ipc) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Control_free, &server->control);

    if (server->control.listen_fd < 0 || (size_t)server->control.listen_fd >= epoll_capacity) {
        LOGGER_LOG(LOG_ERROR, "server", "control listen fd out of table range");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    if (Epoll_accept_one(&server->epoll, server->acceptor.listen_fd, EPOLL_ENTRY_ACCEPTOR, EPOLLIN) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    if (Epoll_accept_one(&server->epoll, server->control.listen_fd, EPOLL_ENTRY_CONTROL_LISTENER, EPOLLIN) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    DTOR_RETURN(dtor, 0);
}

void server_free(Server* server) {
    AppData_free(&server->app);
    // before Epoll_free (which closes control conn fds this layer references)
    // and before config_var_free (control.ipc_path points into cfg)
    Control_free(&server->control);
    HtttpData_free(&server->htttp);
    AuthData_free(&server->auth);
    PlayerIo_free(&server->player_io);
    Epoll_free(&server->epoll);
    Acceptor_free(&server->acceptor);
    config_var_free(&server->cfg);
}

/*
    Shared by both state-dump paths and by neither's delivery: it formats and
    nothing else, so answering GET /status and logging on SIGUSR1 stay separate
    operations over one serialization.
*/
static int render_state_json(const Server* server, char* buf, size_t buf_size, size_t* out_len) {
    size_t players_authed = 0;
    for (size_t i = 0; i < SparseSet_AuthEntry_size(&server->auth.entries); i++) {
        if (SparseSet_AuthEntry_at_idx(&server->auth.entries, i)->auth_state == AUTH_DONE) {
            players_authed++;
        }
    }

    const int written = snprintf(buf, buf_size,
        "{\"pid\":%ld,\"players_connected\":%zu,"
        "\"players_authed\":%zu,\"players_capacity\":%u,"
        "\"fds_used\":%zu,\"fds_capacity\":%zu,\"listen_port\":%d}",
        (long)getpid(), SparseSet_PlayerIoEntry_size(&server->player_io.entries),
        players_authed, server->cfg.max_player_fd,
        SparseSet_EpollEntry_size(&server->epoll.entries),
        server->epoll.entries.capacity, server->cfg.port);
    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }
    *out_len = (size_t)written;
    return 0;
}

static void dump_state_to_log(const Server* server) {
    char buf[512];
    size_t len = 0;
    if (render_state_json(server, buf, sizeof(buf), &len) == -1) {
        LOGGER_LOG(LOG_WARN, "server", "state dump does not fit its buffer");
        return;
    }
    LOGGER_LOG(LOG_INFO, "server", "state %s", buf);
}

static void swap_str(char** a, char** b) {
    char* const tmp = *a;
    *a = *b;
    *b = tmp;
}

void server_reload_config(Server* server) {
    struct config_var new_cfg;
    if (config_var_init(&new_cfg) == -1) {
        LOGGER_LOG(LOG_WARN, "server", "config reload failed to validate; keeping the running config");
        return;
    }

    if (AuthData_reload_credential(&server->auth, new_cfg.key_path, new_cfg.cert_path) == -1) {
        LOGGER_LOG(LOG_WARN, "server", "credential reload failed; keeping the running config");
        config_var_free(&new_cfg);
        return;
    }

    swap_str(&server->cfg.cert_path, &new_cfg.cert_path);
    swap_str(&server->cfg.key_path, &new_cfg.key_path);
    swap_str(&server->cfg.log_ipc, &new_cfg.log_ipc);

    server->cfg.client_capacity = new_cfg.client_capacity;
    server->cfg.max_player_fd = new_cfg.max_player_fd;
    server->cfg.room_tick_hz = new_cfg.room_tick_hz;
    server->cfg.logger_reconnect_seconds = new_cfg.logger_reconnect_seconds;
    clamp_max_player_fd(&server->cfg);

    config_var_free(&new_cfg);

    LOGGER_LOG(LOG_INFO, "server",
               "config reloaded: credentials, log_ipc, logger_reconnect_seconds=%u, "
               "client_capacity=%u, max_player_fd=%u, room_tick_hz=%u",
               server->cfg.logger_reconnect_seconds, server->cfg.client_capacity,
               server->cfg.max_player_fd, server->cfg.room_tick_hz);
}

int server_tick(Server* server) {
    if (!running) {
        return -1;
    }
    if (should_reload_config) {
        should_reload_config = 0;
        server_reload_config(server);
    }
    if (dump_state) {
        dump_state = 0;
        dump_state_to_log(server);
    }

    EpollSignals signals = {0};
    if (Epoll_poll(&server->epoll, &server->player_io.players_reading,
                   &server->player_io.players_writing, &signals) == -1) {
        // interrupted by a signal, or a poll error: the flags above are re-read
        // on the next call, so this is not a stop
        return 0;
    }

    ControlActions actions = {0};
    bool should_stop_accepting = false;
    if (signals.acceptor_readable) {
        Acceptor_accept(&server->acceptor, server->cfg.max_player_fd, &server->acceptor.accepted, &should_stop_accepting);
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

    if (signals.control_listener_readable) {
        const Fd control_fd = Control_accept(&server->control, server->epoll.entries.capacity);
        if (control_fd != -1 &&
            Epoll_accept_one(&server->epoll, control_fd, EPOLL_ENTRY_CONTROL, EPOLLIN) == -1) {
            *SparseSet_bool_activate(&server->epoll.control_close_fds, (size_t)control_fd) = true;
        }
    }

    if (signals.control_hangup) {
        Control_hangup(&server->control, &server->epoll.control_close_fds);
    }
    if (signals.control_readable) {
        Control_read(&server->control, &server->epoll.control_close_fds);
    }

    char state_json[512];
    size_t state_json_len = 0;
    if (Control_has_request(&server->control) &&
        render_state_json(server, state_json, sizeof(state_json), &state_json_len) == -1) {
        state_json_len = 0;
    }
    Control_process(&server->control, state_json, state_json_len,
                    &server->epoll.control_close_fds, &actions);

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

    ControlInterest control_interest;
    Control_write(&server->control, &server->epoll.control_close_fds,
                  &control_interest, &actions);
    if (control_interest.fd != -1) {
        Epoll_set_interest(&server->epoll, control_interest.fd, control_interest.interest);
    }

    Epoll_sync_interest(&server->epoll, &server->player_io.vec_write_qs_status,
                        &server->epoll.player_close_fds,
                        server->acceptor.listen_fd, should_stop_accepting);

    PlayerIo_close(&server->player_io, &server->epoll.player_close_fds);
    AuthData_close(&server->auth, &server->epoll.player_close_fds);
    HtttpData_close(&server->htttp, &server->epoll.player_close_fds);
AppData_close(&server->app, &server->epoll.player_close_fds);
    Epoll_close(&server->epoll, &server->epoll.player_close_fds);

    Control_close(&server->control, &server->epoll.control_close_fds);
    Epoll_close(&server->epoll, &server->epoll.control_close_fds);

    Acceptor_reset(&server->acceptor);
    PlayerIo_reset(&server->player_io);
    AuthData_reset(&server->auth);
    HtttpData_reset(&server->htttp);
    Control_reset(&server->control);
    Epoll_reset(&server->epoll);

    // applied at the top of the next tick, once this one has flushed the
    // response acknowledging it
    if (actions.reload_config) {
        should_reload_config = 1;
    }
    return actions.shutdown ? -1 : 0;
}
