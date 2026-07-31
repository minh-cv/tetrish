#ifndef TETRISH_TETRISD_TYPE_H
#define TETRISH_TETRISD_TYPE_H

#include "config_var.h"
#include "htttp.h" // IWYU pragma: keep
#include "network/reader.h"
#include "network/writer.h"
#include "tetrisbrain/input.h"
#include "tetrisbrain/state.h"
#include "tetrissh.h"
#include <openssl/rand.h>
#include <stdint.h>

typedef enum {
    CONN_INACTIVE = 0,
    CONN_ACCEPTOR,
    CONN_ROOM_TIMER,
    CONN_LOGGER_TIMER,
    CONN_PLAYER,
    CONN_LOGGER,
} EpollConnType;

/*!
    @note `events` mirrors the mask currently registered with epoll, so write
    interest can be armed/disarmed without a redundant `epoll_ctl` every poll.
*/
typedef struct {
    int fd;
    EpollConnType type;
    uint32_t events;
} EpollConn;

#define ROOM_CODE_MAX 7

typedef char RoomCode[ROOM_CODE_MAX];

#define PLAYER_NAME_MAX 21

/*!
    @note fd is -1 while disconnected; the logger timer retries.
*/
typedef struct {
    Writer writer;
    WriterFrameQueue write_queue;
    int fd;
} LoggerFdData;

/*!
    @note fd is -1 when logging is disabled entirely.
*/
typedef struct {
    int fd;
} LoggerTimerFdData;

typedef struct {
    int fd;
} AcceptorFdData;

typedef struct {
    uint8_t max_players;
} RoomConfig;

typedef struct {
    State state;
    bool inputs[PLAYER_INPUT_KEY_COUNT];
    bool is_alive;
} RoomSeat;

#define SPAN_ELEM_TYPE RoomSeat
#define SPAN_TYPEDEF RoomSeats
#include "collection/span.h"

typedef struct {
    RoomSeats seats;
    int host_fd;
    RoomConfig config;
    bool is_game_started;
} Room;

typedef struct {
    Room* room;
    int fd;
} RoomTimerFdData;

typedef enum {
    PLAYER_AUTH_NONCE,
    PLAYER_AUTH_SYMKEY,
    PLAYER_AUTH_DONE,
} PlayerAuthState;

typedef struct {
    Reader reader;
    Writer writer;
    WriterFrameQueue write_queue;
    Room* room;
    SessionKey key;
    char name[PLAYER_NAME_MAX];
    PlayerAuthState auth_state;
    int fd;
} PlayerFdData;

#define SPAN_ELEM_TYPE Room
#define SPAN_TYPEDEF Rooms
#include "collection/span.h"

#define SPAN_ELEM_TYPE PlayerFdData
#define SPAN_TYPEDEF PlayerFds
#include "collection/span.h"

#define SPAN_ELEM_TYPE EpollConn
#define SPAN_TYPEDEF EpollConns
#include "collection/span.h"

#define SPAN_ELEM_TYPE RoomTimerFdData
#define SPAN_TYPEDEF RoomTimerFdDatas
#include "collection/span.h"

typedef struct {
    Rooms rooms;
    PlayerFds players;
    TetrishCredential credential;

    EpollConns conns;
    LoggerFdData logger_fd;
    RoomTimerFdDatas room_timerfds;
    LoggerTimerFdData logger_timerfd;
    AcceptorFdData acceptor_fd;
    struct config_var cfg;
    int epoll_fd;
} Server;

#endif
