#ifndef TETRISH_TETRISU_APP_H
#define TETRISH_TETRISU_APP_H

#include "command/router.h"
#include "net/event.h"
#include "net/message.h"

#include <stdbool.h>
#include <stddef.h>

#define APP_EFFECT_LIST_CAPACITY 4u
#define APP_NOTIFICATION_CAPACITY 256u

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

/*!
    @invariant a non-idle request exists only while the connection is ready
    @invariant @c remote_state and @c last_message are uniquely owned snapshots
*/
typedef struct {
    AppConnectionState connection;
    AppRequestState request;
    OwnedBytes remote_state;
    OwnedBytes last_message;
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

typedef struct {
    AppEffectType type;
    const char* method;
    const char* path;
    const char* content_type;
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
    const OwnedBytes* remote_state;
    const OwnedBytes* last_message;
    const char* notification;
    bool quit_requested;
} AppView;

/*!
    @brief initialize application state and an empty effect list

    @pre @p app is not initialized
    @post @p app owns no message allocations and starts disconnected/idle
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
    @post @p event and all referenced payloads remain owned by their caller
    @post emitted effects own their payloads; request metadata borrows string literals

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
