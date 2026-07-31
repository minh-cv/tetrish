#include "config_var.h"
#include "logger.h"
#include "network/reader.h"
#include "network/writer.h"
#include "player.h"
#include "type.h"
#include "socket.h"
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include "dtor.h"
#include "tetrissh.h"
#include "server.h"

#include <sys/un.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static WriterFrameQueue* log_buffer_ref;

static int close_ptr(const int* fd) {
    return close(*fd);
}

static void free_ptr(void** ptr) {
    free(*ptr);
}

static void log_buffer_detach(WriterFrameQueue** ref) {
    (void)ref;
    logger_set_log_handler(logger_log_null);
    log_buffer_ref = NULL;
}

/*!
    @note `WriterFrameQueue_free` only releases the ring buffer itself, so the
    log lines still queued in it have to go first or they leak.
*/
static void log_buffer_free(WriterFrameQueue* queue) {
    if (queue->data == NULL) {
        return;
    }

    while (!WriterFrameQueue_empty(queue)) {
        free((void*)WriterFrameQueue_front(queue)->ptr);
        WriterFrameQueue_pop_front(queue);
    }
    WriterFrameQueue_free(queue);
}

static DTOR_WRAPPER_DEFINE(log_buffer_free)
static DTOR_WRAPPER_DEFINE(tetrish_credential_free)
static DTOR_WRAPPER_DEFINE(close_ptr)
static DTOR_WRAPPER_DEFINE(log_buffer_detach)
static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(free_ptr)
static DTOR_WRAPPER_DEFINE(writer_free)
static DTOR_WRAPPER_DEFINE(WriterFrameQueue_free)
static DTOR_WRAPPER_DEFINE(reader_free)

/*!
    @note does not take ownership of `fd`; the caller closes it on failure.
*/
static int conn_register(int epoll_fd, EpollConns* conns, int fd, EpollConnType type, uint32_t events) {
    if (fd < 0 || (size_t)fd >= conns->length) {
        LOGGER_LOG(LOG_ERROR, "epoll", "fd=%d out of range", fd);
        return -1;
    }

    EpollConn* c = EpollConns_at(conns, (size_t)fd);
    assert(c->type == CONN_INACTIVE);
    if (c->type != CONN_INACTIVE) {
        LOGGER_LOG(LOG_ERROR, "epoll", "fd=%d leaked", fd);
        return -1;
    }

    struct epoll_event ev = {
        events,
        {
            .ptr = c,
        }
    };

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        LOGGER_PERROR("epoll", "epoll_ctl add");
        return -1;
    }

    c->fd = fd;
    c->type = type;
    c->events = events;

    return 0;
}

static int conn_mod(int epoll_fd, EpollConn* c, uint32_t events) {
    assert(c->type != CONN_INACTIVE);

    struct epoll_event ev = {
        events,
        {
            .ptr = c,
        }
    };

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
        LOGGER_PERROR("epoll", "epoll_ctl mod");
        return -1;
    }

    c->events = events;

    return 0;
}

static int reconnect_client_logger(Server* server) {
    LoggerFdData* logger = &server->logger_fd;
    assert(logger->fd == -1);

    int client_logger_fd = prepare_logger_socket(server->cfg.log_ipc);
    if (client_logger_fd == -1) {
        return -1;
    }

    // write interest is armed on demand; a permanently armed EPOLLOUT would
    // spin the loop whenever there is nothing to log.
    if (conn_register(server->epoll_fd, &server->conns, client_logger_fd, CONN_LOGGER, EPOLLRDHUP) == -1) {
        close(client_logger_fd);
        return -1;
    }

    writer_free(&logger->writer);
    writer_init(&logger->writer);
    logger->writer.max_frames_allowed = server->cfg.logger_capacity;
    logger->fd = client_logger_fd;

    return 0;
}

static int logger_handler_log_buf(char* string) {
    assert(log_buffer_ref != NULL);
    if (string == NULL) {
        return -1;
    }

    WriterFrame frame = {
        (const unsigned char*)string,
        strlen(string),
    };

    // the handler owns the string, so a full queue has to drop it, not leak it.
    if (frame.length == 0 || frame.length > FRAME_MAX ||
        WriterFrameQueue_push_back(log_buffer_ref, &frame) == -1) {
        free(string);
        return -1;
    }

    return 0;
}

static void accept_until_block(Server* server) {
    int listen_fd = server->acceptor_fd.fd;
    const struct config_var* cfg = &server->cfg;
    PlayerFds* player_fds = &server->players;
    EpollConns* connections = &server->conns;

    for (;;) {
        DTOR_DEFINE(dtor, 10);
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                DTOR_BREAK(dtor);
            }
            if (errno == ECONNABORTED) {
                DTOR_CONTINUE(dtor);
            }
            LOGGER_PERROR("accept", "accept");
            DTOR_BREAK(dtor);
        }
        DTOR_INSERT(dtor, close_ptr, &client_fd);

        // TODO: disarm once full until a client closes.
        if ((unsigned int)client_fd >= cfg->max_fds) {
            LOGGER_LOG(LOG_ERROR, "accept", "client_fd=%d too large", client_fd);
            DTOR_BREAK(dtor);
        }

        if (set_nonblocking(client_fd) == -1) {
            LOGGER_PERROR("accept", "set_nonblocking");
            DTOR_CONTINUE(dtor);
        }
        
        PlayerFdData* slot = PlayerFds_at(player_fds, (size_t)client_fd);
        EpollConn* c = EpollConns_at(connections, (size_t)client_fd);
        assert(c->type == CONN_INACTIVE);
        if (c->type != CONN_INACTIVE) {
            LOGGER_LOG(LOG_ERROR, "accept", "fd=%d leaked", client_fd);
            DTOR_CONTINUE(dtor);
        }

        slot->auth_state = PLAYER_AUTH_NONCE;
        writer_init(&slot->writer);
        slot->writer.max_frames_allowed = cfg->client_capacity;
        DTOR_INSERT(dtor, writer_free, &slot->writer);

        if (reader_init(&slot->reader, cfg->client_capacity) == -1) {
            DTOR_CONTINUE(dtor);
        }
        slot->reader.max_frames_allowed = cfg->client_capacity;
        DTOR_INSERT(dtor, reader_free, &slot->reader);

        if (WriterFrameQueue_init(&slot->write_queue, cfg->client_capacity) == -1) {
            DTOR_CONTINUE(dtor);
        }
        DTOR_INSERT(dtor, WriterFrameQueue_free, &slot->write_queue);
    
        if (conn_register(server->epoll_fd, connections, client_fd, CONN_PLAYER, EPOLLIN | EPOLLRDHUP) == -1) {
            DTOR_CONTINUE(dtor);
        }

        // claimed last, so every failure above leaves the slot reading as free.
        slot->fd = client_fd;

        LOGGER_LOG(LOG_INFO, "client", "accept client fd=%d", client_fd);
    }
}

void server_reload_config(Server* server) {
    struct config_var* cfg = &server->cfg;
    TetrishCredential* const credential = &server->credential;

    LOGGER_LOG(LOG_INFO, "config", "Reconfiguring...");
    struct config_var tmp_cfg;
    if (config_var_init(&tmp_cfg) == -1) {
        return;
    }

    if (tmp_cfg.port != cfg->port) {
        LOGGER_LOG(LOG_WARN, "config", "Ignoring new port");
    }

    if (strcmp(tmp_cfg.address, cfg->address) != 0) {
        char* tmp = cfg->address;
        cfg->address = tmp_cfg.address;
        tmp_cfg.address = tmp;
    }

    if (strcmp(tmp_cfg.key_path, cfg->key_path) != 0 || strcmp(tmp_cfg.cert_path, cfg->cert_path) != 0) {
        TetrishCredential new_credential;
        if (tetrish_credential_init(&new_credential, tmp_cfg.key_path, tmp_cfg.cert_path) == -1) {
            LOGGER_LOG(LOG_ERROR, "config", "Cannot reconfigure new credential path");
        }
        else {
            tetrish_credential_free(credential);
            *credential = new_credential;
        }
    }

    if (tmp_cfg.max_fds != cfg->max_fds) {
        LOGGER_LOG(LOG_WARN, "config", "Ignoring max_fds");
    }

    if (strcmp(tmp_cfg.log_ipc, cfg->log_ipc) != 0) {
        char* tmp = tmp_cfg.log_ipc;
        tmp_cfg.log_ipc = cfg->log_ipc;
        cfg->log_ipc = tmp;
    }

    if (tmp_cfg.max_events != cfg->max_events) {
        LOGGER_LOG(LOG_WARN, "config", "Ignoring max_events");
    }

    config_var_free(&tmp_cfg);
    return;
}

static int server_close(Server* server, int fd) {
    assert(0 <= fd && (size_t)fd < server->conns.length);
    if (!(0 <= fd && (size_t)fd < server->conns.length)) {
        return -1;
    }

    EpollConn* c = EpollConns_at(&server->conns, (size_t)fd);

    switch (c->type) {
    case CONN_INACTIVE:
        assert(false);
        return -1;
    case CONN_ACCEPTOR:
        server->acceptor_fd.fd = -1;
        break;
    case CONN_ROOM_TIMER:
        RoomTimerFdDatas_at(&server->room_timerfds, (size_t)fd)->fd = -1;
        break;
    case CONN_LOGGER_TIMER:
        server->logger_timerfd.fd = -1;
        break;
    case CONN_PLAYER:
        player_slot_free(PlayerFds_at(&server->players, (size_t)fd));
        break;
    case CONN_LOGGER:
        // write_queue outlives the socket so a reconnect keeps the buffered logs.
        writer_free(&server->logger_fd.writer);
        server->logger_fd.fd = -1;
        break;
    }

    // reset before close, or a reused fd number lands on a slot still tagged.
    c->type = CONN_INACTIVE;
    c->fd = -1;

    if (close(fd) == -1) {
        LOGGER_PERROR("main", "close");
        return -1;
    }

    return 0;
}

static bool logger_wants_write(const LoggerFdData* logger) {
    return logger->writer.state != WRITER_IDLE || !WriterFrameQueue_empty(&logger->write_queue);
}

int server_init(Server* server) {
    DTOR_DEFINE(errdtor, 16);
    struct config_var cfg;
    if (config_var_init(&cfg) == -1) {
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, config_var_free, &cfg);

    WriterFrameQueue logger_write_queue = {0};
    bool is_logging_enabled = false;

    if (WriterFrameQueue_init(&logger_write_queue, cfg.logger_capacity) != -1) {
        // anything logged before a failed init is still queued, so drain, don't just free.
        DTOR_INSERT(errdtor, log_buffer_free, &logger_write_queue);
        log_buffer_ref = &logger_write_queue;
        logger_set_log_handler(logger_handler_log_buf);
        is_logging_enabled = true;
        // must unwind before the queue it points at.
        DTOR_INSERT(errdtor, log_buffer_detach, &log_buffer_ref);
    }

    TetrishCredential credential;
    if (tetrish_credential_init(&credential, cfg.key_path, cfg.cert_path) == -1) {
        LOGGER_PERROR("auth", "credential init");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, tetrish_credential_free, &credential);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        LOGGER_PERROR("epoll", "epoll_create1");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, close_ptr, &epoll_fd);

    // conns and players are both indexed by fd, so both span max_fds.
    EpollConns conns = {0};
    if (EpollConns_calloc(&conns, cfg.max_fds) == -1) {
        LOGGER_PERROR("main", "calloc conns");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, free_ptr, &conns.ptr);

    PlayerFds players = {0};
    if (PlayerFds_calloc(&players, cfg.max_fds) == -1) {
        LOGGER_PERROR("main", "calloc players");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, free_ptr, &players.ptr);

    // 0 is a valid fd, so an unoccupied slot has to say so explicitly.
    for (size_t i = 0; i < players.length; i++) {
        PlayerFds_at(&players, i)->fd = -1;
    }

    Rooms rooms = {0};
    if (Rooms_calloc(&rooms, cfg.max_rooms) == -1) {
        LOGGER_PERROR("main", "calloc rooms");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, free_ptr, &rooms.ptr);

    for (size_t i = 0; i < rooms.length; i++) {
        Rooms_at(&rooms, i)->host_fd = -1;
    }

    RoomTimerFdDatas room_timerfds = {0};

    if (RoomTimerFdDatas_calloc(&room_timerfds, cfg.max_fds) == -1) {
        LOGGER_PERROR("main", "calloc room_timerfds");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, free_ptr, &room_timerfds.ptr);

    for (size_t i = 0; i < room_timerfds.length; i++) {
        RoomTimerFdDatas_at(&room_timerfds, i)->fd = -1;
    }

    int listen_fd = prepare_socket(cfg.address, cfg.port);
    if (listen_fd == -1) {
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, close_ptr, &listen_fd);

    if (conn_register(epoll_fd, &conns, listen_fd, CONN_ACCEPTOR, EPOLLIN | EPOLLRDHUP) == -1) {
        DTOR_RETURN(errdtor, -1);
    }

    // no EPOLL_CTL_DEL on the error path: closing epoll_fd drops every
    // registration with it.

    int logger_timerfd = -1;
    if (is_logging_enabled) {
        logger_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (logger_timerfd == -1) {
            LOGGER_PERROR("logger", "timerfd_create");
            DTOR_RETURN(errdtor, -1);
        }
        DTOR_INSERT(errdtor, close_ptr, &logger_timerfd);

        struct itimerspec interval = {
            {(time_t)cfg.logger_reconnect_seconds, 0},
            {(time_t)cfg.logger_reconnect_seconds, 0},
        };

        if (timerfd_settime(logger_timerfd, 0, &interval, NULL) == -1) {
            LOGGER_PERROR("logger", "timerfd_settime");
            DTOR_RETURN(errdtor, -1);
        }

        if (conn_register(epoll_fd, &conns, logger_timerfd, CONN_LOGGER_TIMER, EPOLLIN) == -1) {
            DTOR_RETURN(errdtor, -1);
        }
    }

    Server new_server = {
        .rooms = rooms,
        .players = players,
        .credential = credential,
        .conns = conns,
        .logger_fd = {
            .write_queue = logger_write_queue,
            .fd = -1,
        },
        .room_timerfds = room_timerfds,
        .logger_timerfd = {
            .fd = logger_timerfd,
        },
        .acceptor_fd = {
            .fd = listen_fd,
        },
        .cfg = cfg,
        .epoll_fd = epoll_fd,
    };
    *server = new_server;

    if (is_logging_enabled) {
        log_buffer_ref = &server->logger_fd.write_queue;
        writer_init(&server->logger_fd.writer);
        if (reconnect_client_logger(server) == -1) {
            LOGGER_LOG(LOG_WARN, "logger", "log_ipc unavailable, retrying every %us", cfg.logger_reconnect_seconds);
        }
    }

    LOGGER_LOG(LOG_INFO, "main", "server listening on port %d", cfg.port);

    return 0;
}

/*!
    @note discards whatever is still queued for tetrislogd; flush before calling.
*/
void server_free(Server* server) {
    for (size_t fd = 0; fd < server->conns.length; fd++) {
        if (EpollConns_at(&server->conns, fd)->type != CONN_INACTIVE) {
            server_close(server, (int)fd);
        }
    }

    for (size_t i = 0; i < server->rooms.length; i++) {
        Room* room = Rooms_at(&server->rooms, i);
        free(room->seats.ptr);
        room->seats.ptr = NULL;
        room->seats.length = 0;
    }

    tetrish_credential_free(&server->credential);

    // past this point nothing may log: the sink is about to be freed.
    log_buffer_detach(&log_buffer_ref);

    log_buffer_free(&server->logger_fd.write_queue);

    free(server->conns.ptr);
    free(server->players.ptr);
    free(server->rooms.ptr);
    free(server->room_timerfds.ptr);

    if (server->epoll_fd != -1) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }

    config_var_free(&server->cfg);
}

static void timer_consume(int fd) {
    uint64_t expirations;
    ssize_t n = read(fd, &expirations, sizeof(expirations));
    if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOGGER_PERROR("timer", "read timerfd");
    }
}

/*!
    @return 0 to keep the connection, -1 to have the caller close it.
*/
static int handle_player_event(Server* server, PlayerFdData* player, uint32_t ev) {
    if (ev & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
        return -1;
    }

    bool closing = false;

    if (ev & EPOLLIN) {
        // an error here still leaves already-completed frames worth serving.
        closing = reader_recv(&player->reader, player->fd) == -1;
    }

    if (player_process(server, player) == -1) {
        closing = true;
    }

    if (!closing && (ev & EPOLLOUT) &&
            writer_send(&player->writer, player->fd, &player->write_queue) == -1) {
        closing = true;
    }

    return closing ? -1 : 0;
}

/*!
    @return 0 to keep the connection, -1 to have the caller close it.
    @note no read interest is registered, so a hangup is the only thing tetrislogd
    can tell us besides writability.
*/
static int handle_logger_event(Server* server, uint32_t ev) {
    LoggerFdData* logger = &server->logger_fd;

    if (ev & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
        return -1;
    }

    if ((ev & EPOLLOUT) &&
            writer_send(&logger->writer, logger->fd, &logger->write_queue) == -1) {
        return -1;
    }

    return 0;
}

/*!
    @brief Arm EPOLLOUT exactly on the connections that have something buffered.

    @note a permanently armed EPOLLOUT would make every idle poll return
    immediately, and arming at push time would miss whatever other connections
    enqueue for each other.
*/
static void server_sync_write_interest(Server* server) {
    for (size_t fd = 0; fd < server->conns.length; fd++) {
        EpollConn* conn = EpollConns_at(&server->conns, fd);

        bool wants_write;
        switch (conn->type) {
        case CONN_PLAYER:
            wants_write = player_wants_write(PlayerFds_at(&server->players, fd));
            break;
        case CONN_LOGGER:
            wants_write = logger_wants_write(&server->logger_fd);
            break;
        default:
            continue;
        }

        uint32_t events = wants_write ? (conn->events | EPOLLOUT) : (conn->events & ~(uint32_t)EPOLLOUT);
        if (events != conn->events) {
            conn_mod(server->epoll_fd, conn, events);
        }
    }
}

int server_poll(Server* server, EpollEvents* evs) {
    // before blocking, not after: anything buffered by the previous pass (or by
    // startup) has to be armed or epoll_wait sleeps on it forever.
    server_sync_write_interest(server);

    int n = epoll_wait(server->epoll_fd, evs->ptr, (int)server->cfg.max_events, -1);
    if (n == -1) {
        // a signal is not a failure: the caller re-checks its flags and polls again.
        if (errno == EINTR) {
            return -1;
        }
        LOGGER_PERROR("epoll", "epoll_wait");
        return -1;
    }

    for (size_t i = 0; i < (size_t)n; i++) {
        struct epoll_event* epoll_ev = EpollEvents_at(evs, i);
        EpollConn* conn = (EpollConn*)epoll_ev->data.ptr;
        uint32_t ev = epoll_ev->events;
        int fd = conn->fd;

        switch (conn->type) {
        case CONN_ACCEPTOR:
            accept_until_block(server);
            break;
        case CONN_ROOM_TIMER:
            timer_consume(fd);
            // TODO: step the room's game state once libtetrisbrain is wired in.
            break;
        case CONN_LOGGER_TIMER:
            timer_consume(fd);
            if (server->logger_fd.fd == -1) {
                reconnect_client_logger(server);
            }
            break;
        case CONN_PLAYER: {
            PlayerFdData* player = PlayerFds_at(&server->players, (size_t)fd);
            if (handle_player_event(server, player, ev) == -1) {
                LOGGER_LOG(LOG_INFO, "client", "close client fd=%d", fd);
                server_close(server, fd);
            }
            break;
        }
        case CONN_LOGGER:
            assert(server->logger_fd.fd == fd);
            if (handle_logger_event(server, ev) == -1) {
                // the reconnect timer stays armed, so the next tick retries.
                server_close(server, fd);
            }
            break;
        case CONN_INACTIVE:
        default:
            assert(false);
            break;
        }
    }

    return 0;
}
