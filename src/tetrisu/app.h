#ifndef TETRISH_TETRISU_APP_H
#define TETRISH_TETRISU_APP_H

#include "command/router.h"
#include "game/intent.h"
#include "net/event.h"
#include "net/message.h"
#include "proto.h"

#include <stdbool.h>
#include <stddef.h>

#define APP_EFFECT_LIST_CAPACITY 4u
#define APP_NOTIFICATION_CAPACITY 256u
#define APP_GAME_INPUT_QUEUE_CAPACITY 32u

typedef enum {
    APP_CONNECTION_DISCONNECTED,
    APP_CONNECTION_CONNECTING,
    APP_CONNECTION_HANDSHAKING,
    APP_CONNECTION_READY,
} AppConnectionState;

typedef enum {
    APP_REQUEST_IDLE,
    APP_REQUEST_SUBMITTING,
    APP_REQUEST_PENDING,
} AppRequestState;

typedef enum {
    APP_GAME_NO_ROOM,
    APP_GAME_LOBBY,
    APP_GAME_ACTIVE,
} AppGamePhase;

typedef enum {
    APP_PENDING_NONE,
    APP_PENDING_CREATE,
    APP_PENDING_JOIN,
    APP_PENDING_START,
    APP_PENDING_LEAVE,
} AppPendingOperation;

/*!
    @invariant a non-idle request exists only while the connection is ready
    @invariant queued intents are all one-way gameplay inputs
    @invariant @c input_count does not exceed @c APP_GAME_INPUT_QUEUE_CAPACITY
    @invariant @c last_message is uniquely owned
    @invariant @c game_state is meaningful iff @c has_game_state is true
*/
typedef struct {
    AppConnectionState connection;
    AppRequestState request;
    AppGamePhase game_phase;
    AppPendingOperation pending_operation;
    ClientRequestCompletion active_completion;
    GameIntentType input_queue[APP_GAME_INPUT_QUEUE_CAPACITY];
    size_t input_head;
    size_t input_count;
    ProtoStateRequest game_state;
    bool has_game_state;
    /*!
        @brief the room the player is in, as the server reported it

        Taken from the body of a successful CREATE or JOIN, since telling
        somebody else the code is the whole of the lobby flow.
    */
    size_t room_id;
    bool has_room_id;
    OwnedBytes last_message;
    int last_response_status;
    char notification[APP_NOTIFICATION_CAPACITY];
    bool quit_requested;
    bool view_dirty;
} AppState;

typedef enum {
    APP_EVENT_START,
    APP_EVENT_COMMAND_SUBMITTED,
    APP_EVENT_NETWORK,
} AppEventType;

typedef struct {
    AppEventType type;
    union {
        const ParsedCommand* command;
        const NetEvent* network;
    } data;
} AppEvent;

typedef enum {
    APP_EFFECT_NET_CONNECT,
    APP_EFFECT_NET_SEND,
    APP_EFFECT_NET_DISCONNECT,
    APP_EFFECT_QUIT,
} AppEffectType;

//! @brief room for `/room/<id>` at any id the wire can carry
#define APP_EFFECT_PATH_CAPACITY 32u

/*!
    @invariant @c method and @c content_type borrow literals and outlive the
               effect list
    @invariant @c payload is uniquely owned

    @note @c path is stored inline rather than borrowed: a room id makes it
          per-request, so there is no literal to point at and the builder's
          scratch is gone by the time the effect runs.
*/
typedef struct {
    AppEffectType type;
    const char* method;
    char path[APP_EFFECT_PATH_CAPACITY];
    const char* content_type;
    ClientRequestCompletion completion;
    OwnedBytes payload;
} AppEffect;

/*!
    @invariant entries below @c count uniquely own their payloads
*/
typedef struct {
    AppEffect items[APP_EFFECT_LIST_CAPACITY];
    size_t count;
} AppEffectList;

typedef struct {
    AppConnectionState connection;
    AppRequestState request;
    AppGamePhase game_phase;
    const ProtoStateRequest* game_state;
    bool has_game_state;
    size_t room_id;
    bool has_room_id;
    const OwnedBytes* last_message;
    int last_response_status;
    size_t queued_inputs;
    const char* notification;
    bool quit_requested;
} AppView;

/*!
    @brief initialize application state
    @pre @p app is not initialized
    @post @p app owns no allocation and starts disconnected, idle and outside a room
*/
void app_init(AppState* app);

/*!
    @brief release all allocations owned by @p app
    @pre @p app has not already been freed
    @post @p app owns no allocation and may be freed again
*/
void app_free(AppState* app);

/*!
    @brief initialize an empty effect list
    @post @p effects owns no payload and has count `0`
*/
void app_effect_list_init(AppEffectList* effects);

/*!
    @brief release every effect payload in @p effects
    @post @p effects has count `0` and may be freed again
*/
void app_effect_list_free(AppEffectList* effects);

/*!
    @brief reduce one application event into state changes and external effects

    @pre @p app is initialized
    @pre @p effects is initialized and empty
    @pre payload referenced by @p event remains valid for this call
    @post only @p app and @p effects may change
    @post emitted effects own their payloads

    @return `0` on success, `-1` if an effect/payload cannot be allocated
*/
int app_reduce(AppState* app, const AppEvent* event, AppEffectList* effects);

/*!
    @brief build a borrowed immutable view over @p app
    @pre @p app is initialized
    @post returned pointers remain valid until @p app is next modified
*/
void app_build_view(const AppState* app, AppView* view);

#endif
