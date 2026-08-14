#include "app.h"

#include "game/request.h"

#include <stdio.h>
#include <string.h>

static void set_notification(AppState* app, const char* text) {
    (void)snprintf(app->notification, sizeof(app->notification), "%s", text);
    app->view_dirty = true;
}

static int replace_bytes(OwnedBytes* destination, const OwnedBytes* source) {
    OwnedBytes replacement;
    if (owned_bytes_copy(&replacement, source->ptr, source->len) == -1) {
        return -1;
    }
    owned_bytes_free(destination);
    owned_bytes_move(destination, &replacement);
    return 0;
}

static int push_effect(
    AppEffectList* effects,
    AppEffectType type,
    const char* method,
    const char* path,
    const char* content_type,
    ClientRequestCompletion completion,
    const void* payload,
    size_t payload_len
) {
    if (effects->count == APP_EFFECT_LIST_CAPACITY) {
        return -1;
    }
    AppEffect effect = {
        .type = type,
        .method = method,
        .content_type = content_type,
        .completion = completion,
    };
    if (path != NULL) {
        const size_t length = strlen(path);
        if (length >= sizeof(effect.path)) {
            return -1;
        }
        memcpy(effect.path, path, length + 1);
    }
    owned_bytes_init(&effect.payload);
    if (owned_bytes_copy(&effect.payload, payload, payload_len) == -1) {
        return -1;
    }
    effects->items[effects->count++] = effect;
    return 0;
}

static void clear_input_queue(AppState* app) {
    app->input_head = 0;
    app->input_count = 0;
}

static void clear_game_session(AppState* app) {
    app->request = APP_REQUEST_IDLE;
    app->game_phase = APP_GAME_NO_ROOM;
    app->pending_operation = APP_PENDING_NONE;
    app->active_completion = CLIENT_REQUEST_EXPECT_REPLY;
    clear_input_queue(app);
    memset(&app->game_state, 0, sizeof(app->game_state));
    app->has_game_state = false;
}

void app_init(AppState* app) {
    memset(app, 0, sizeof(*app));
    app->connection = APP_CONNECTION_DISCONNECTED;
    app->request = APP_REQUEST_IDLE;
    app->game_phase = APP_GAME_NO_ROOM;
    app->active_completion = CLIENT_REQUEST_EXPECT_REPLY;
    owned_bytes_init(&app->last_message);
    set_notification(app, "Starting tetrisu");
}

void app_free(AppState* app) {
    owned_bytes_free(&app->last_message);
    memset(app, 0, sizeof(*app));
}

void app_effect_list_init(AppEffectList* effects) {
    memset(effects, 0, sizeof(*effects));
}

void app_effect_list_free(AppEffectList* effects) {
    for (size_t i = 0; i < effects->count; ++i) {
        owned_bytes_free(&effects->items[i].payload);
    }
    memset(effects, 0, sizeof(*effects));
}

static AppPendingOperation pending_for_intent(GameIntentType intent) {
    switch (intent) {
    case GAME_INTENT_CREATE: return APP_PENDING_CREATE;
    case GAME_INTENT_JOIN: return APP_PENDING_JOIN;
    case GAME_INTENT_START: return APP_PENDING_START;
    case GAME_INTENT_LEAVE: return APP_PENDING_LEAVE;
    default: return APP_PENDING_NONE;
    }
}

static int emit_request(
    AppState* app,
    GameIntentType intent,
    const char* argument,
    AppEffectList* effects
) {
    ClientRequest request;
    GameRequestScratch scratch;
    if (game_request_from_intent(intent, argument, &scratch, &request) == -1) {
        return -1;
    }
    if (push_effect(
        effects, APP_EFFECT_NET_SEND,
        request.method, request.path, request.content_type, request.completion,
        request.body, request.body_len
    ) == -1) {
        return -1;
    }
    app->request = APP_REQUEST_SUBMITTING;
    app->active_completion = request.completion;
    app->pending_operation = pending_for_intent(intent);
    app->view_dirty = true;
    return 0;
}

static int enqueue_input(AppState* app, GameIntentType intent) {
    if (app->input_count == APP_GAME_INPUT_QUEUE_CAPACITY) {
        return -1;
    }
    const size_t index =
        (app->input_head + app->input_count) % APP_GAME_INPUT_QUEUE_CAPACITY;
    app->input_queue[index] = intent;
    app->input_count++;
    app->view_dirty = true;
    return 0;
}

static int schedule_next_input(AppState* app, AppEffectList* effects) {
    if (app->connection != APP_CONNECTION_READY ||
        app->request != APP_REQUEST_IDLE || app->input_count == 0) {
        return 0;
    }
    // queued intents are gameplay inputs, which never carry an argument
    const GameIntentType intent = app->input_queue[app->input_head];
    if (emit_request(app, intent, NULL, effects) == -1) {
        return -1;
    }
    app->input_head = (app->input_head + 1) % APP_GAME_INPUT_QUEUE_CAPACITY;
    app->input_count--;
    return 0;
}

static int reduce_game_command(
    AppState* app,
    GameIntentType intent,
    const char* argument,
    AppEffectList* effects
) {
    if (app->connection != APP_CONNECTION_READY) {
        set_notification(app, "Not connected");
        return 0;
    }

    if (game_intent_is_input(intent)) {
        if (app->game_phase != APP_GAME_ACTIVE) {
            set_notification(app, "Start a game before sending input");
            return 0;
        }
        if (enqueue_input(app, intent) == -1) {
            set_notification(app, "Gameplay input queue is full");
            return 0;
        }
        return schedule_next_input(app, effects);
    }

    if (app->request != APP_REQUEST_IDLE || app->input_count != 0) {
        set_notification(app, "Network is busy; retry the room command");
        return 0;
    }
    if ((intent == GAME_INTENT_CREATE || intent == GAME_INTENT_JOIN) &&
        app->game_phase != APP_GAME_NO_ROOM) {
        set_notification(app, "Already in a room");
        return 0;
    }
    if (intent == GAME_INTENT_START && app->game_phase != APP_GAME_LOBBY) {
        set_notification(app, "Create or join a room before starting");
        return 0;
    }
    if (intent == GAME_INTENT_LEAVE && app->game_phase == APP_GAME_NO_ROOM) {
        set_notification(app, "Not in a room");
        return 0;
    }
    return emit_request(app, intent, argument, effects);
}

static int reduce_command(
    AppState* app,
    const ParsedCommand* command,
    AppEffectList* effects
) {
    switch (command->type) {
    case COMMAND_HELP:
        set_notification(
            app,
            "create, join, start, move, rotate, drop, hold, leave, reconnect, quit"
        );
        return 0;
    case COMMAND_QUIT:
        app->quit_requested = true;
        app->view_dirty = true;
        return push_effect(
            effects, APP_EFFECT_QUIT, NULL, NULL, NULL,
            CLIENT_REQUEST_EXPECT_REPLY, NULL, 0
        );
    case COMMAND_RECONNECT:
        clear_game_session(app);
        app->connection = APP_CONNECTION_CONNECTING;
        app->view_dirty = true;
        if (push_effect(
            effects, APP_EFFECT_NET_DISCONNECT, NULL, NULL, NULL,
            CLIENT_REQUEST_EXPECT_REPLY, NULL, 0
        ) == -1) {
            return -1;
        }
        return push_effect(
            effects, APP_EFFECT_NET_CONNECT, NULL, NULL, NULL,
            CLIENT_REQUEST_EXPECT_REPLY, NULL, 0
        );
    case COMMAND_DISCONNECT:
        return push_effect(
            effects, APP_EFFECT_NET_DISCONNECT, NULL, NULL, NULL,
            CLIENT_REQUEST_EXPECT_REPLY, NULL, 0
        );
    case COMMAND_GAME:
        return reduce_game_command(app, command->game_intent, command->argument, effects);
    case COMMAND_SEND_RAW:
        if (app->connection != APP_CONNECTION_READY) {
            set_notification(app, "Not connected");
            return 0;
        }
        if (app->request != APP_REQUEST_IDLE || app->input_count != 0) {
            set_notification(app, "Network is busy");
            return 0;
        }
        if (push_effect(
            effects, APP_EFFECT_NET_SEND, "HTTTP", "/", "text/plain",
            CLIENT_REQUEST_EXPECT_REPLY, command->argument, command->argument_len
        ) == -1) {
            return -1;
        }
        app->request = APP_REQUEST_SUBMITTING;
        app->pending_operation = APP_PENDING_NONE;
        app->active_completion = CLIENT_REQUEST_EXPECT_REPLY;
        app->view_dirty = true;
        return 0;
    case COMMAND_UNSUPPORTED:
        set_notification(app, "Command is not supported by the gameplay server");
        return 0;
    }
    return -1;
}

/*!
    @brief read the room id CREATE and JOIN answer with

    The body is the id in decimal and nothing else. A body that is not that
    leaves the id unset rather than guessing: the phase transition does not
    depend on it, so an unreadable code costs the player the shortcut of
    being told their room, not the room.
*/
static void capture_room_id(AppState* app, const OwnedBytes* body) {
    app->has_room_id = false;
    if (body->ptr == NULL || body->len == 0 || body->len > 20) {
        return;
    }
    size_t value = 0;
    for (size_t i = 0; i < body->len; ++i) {
        const unsigned char digit = body->ptr[i];
        if (digit < '0' || digit > '9') {
            return;
        }
        value = value * 10 + (size_t)(digit - '0');
    }
    app->room_id = value;
    app->has_room_id = true;
}

static void apply_response(AppState* app, int status) {
    const bool success = status >= 200 && status < 300;
    if (success) {
        switch (app->pending_operation) {
        case APP_PENDING_CREATE:
        case APP_PENDING_JOIN:
            app->game_phase = APP_GAME_LOBBY;
            app->has_game_state = false;
            capture_room_id(app, &app->last_message);
            break;
        case APP_PENDING_START:
            app->game_phase = APP_GAME_ACTIVE;
            break;
        case APP_PENDING_LEAVE:
            app->game_phase = APP_GAME_NO_ROOM;
            app->has_game_state = false;
            app->has_room_id = false;
            clear_input_queue(app);
            break;
        case APP_PENDING_NONE:
            break;
        }
    }

    char text[APP_NOTIFICATION_CAPACITY];
    if (success && app->has_room_id &&
        (app->pending_operation == APP_PENDING_CREATE ||
         app->pending_operation == APP_PENDING_JOIN)) {
        (void)snprintf(text, sizeof(text), "In room %zu; share that code to be joined",
                       app->room_id);
    }
    else {
        (void)snprintf(text, sizeof(text), "Server response: %d", status);
    }
    set_notification(app, text);
}

static int reduce_network(
    AppState* app,
    const NetEvent* event,
    AppEffectList* effects
) {
    switch (event->type) {
    case NET_EVENT_CONNECTING:
        app->connection = APP_CONNECTION_CONNECTING;
        set_notification(app, "Connecting");
        break;
    case NET_EVENT_HANDSHAKING:
        app->connection = APP_CONNECTION_HANDSHAKING;
        set_notification(app, "Authenticating server");
        break;
    case NET_EVENT_CONNECTED:
        app->connection = APP_CONNECTION_READY;
        set_notification(app, "Connected; create a room to play");
        break;
    case NET_EVENT_SEND_ACCEPTED:
        app->request = event->completion == CLIENT_REQUEST_EXPECT_REPLY
            ? APP_REQUEST_PENDING
            : APP_REQUEST_SUBMITTING;
        app->active_completion = event->completion;
        set_notification(
            app,
            event->completion == CLIENT_REQUEST_EXPECT_REPLY
                ? "Request pending"
                : "Sending gameplay input"
        );
        break;
    case NET_EVENT_SEND_COMPLETED:
        app->request = APP_REQUEST_IDLE;
        app->active_completion = CLIENT_REQUEST_EXPECT_REPLY;
        app->pending_operation = APP_PENDING_NONE;
        set_notification(app, "Gameplay input sent");
        return schedule_next_input(app, effects);
    case NET_EVENT_REPLY:
    case NET_EVENT_ECHO:
        if (replace_bytes(&app->last_message, &event->payload) == -1) {
            return -1;
        }
        app->last_response_status = event->response_status;
        app->request = APP_REQUEST_IDLE;
        if (event->type == NET_EVENT_REPLY) {
            apply_response(app, event->response_status);
        }
        else {
            set_notification(app, "Legacy echo received");
        }
        app->pending_operation = APP_PENDING_NONE;
        app->active_completion = CLIENT_REQUEST_EXPECT_REPLY;
        return schedule_next_input(app, effects);
    case NET_EVENT_STATE_PUSH:
        if (app->game_phase != APP_GAME_NO_ROOM) {
            app->game_state = event->state;
            app->has_game_state = true;
            app->game_phase = event->state.is_game_active
                ? APP_GAME_ACTIVE
                : APP_GAME_LOBBY;
            if (!event->state.is_game_active) {
                clear_input_queue(app);
            }
            set_notification(
                app,
                event->state.is_game_active ? "Game state updated" : "Game over"
            );
        }
        break;
    case NET_EVENT_DISCONNECTED:
        app->connection = APP_CONNECTION_DISCONNECTED;
        clear_game_session(app);
        set_notification(app, "Disconnected");
        break;
    case NET_EVENT_ERROR:
        app->connection = APP_CONNECTION_DISCONNECTED;
        clear_game_session(app);
        set_notification(
            app,
            event->error.detail != NULL ? event->error.detail : "Network error"
        );
        break;
    }
    app->view_dirty = true;
    return 0;
}

int app_reduce(AppState* app, const AppEvent* event, AppEffectList* effects) {
    switch (event->type) {
    case APP_EVENT_START:
        clear_game_session(app);
        app->connection = APP_CONNECTION_CONNECTING;
        app->view_dirty = true;
        return push_effect(
            effects, APP_EFFECT_NET_CONNECT, NULL, NULL, NULL,
            CLIENT_REQUEST_EXPECT_REPLY, NULL, 0
        );
    case APP_EVENT_COMMAND_SUBMITTED:
        return reduce_command(app, event->data.command, effects);
    case APP_EVENT_NETWORK:
        return reduce_network(app, event->data.network, effects);
    }
    return -1;
}

void app_build_view(const AppState* app, AppView* view) {
    *view = (AppView){
        .connection = app->connection,
        .request = app->request,
        .game_phase = app->game_phase,
        .game_state = &app->game_state,
        .has_game_state = app->has_game_state,
        .room_id = app->room_id,
        .has_room_id = app->has_room_id,
        .last_message = &app->last_message,
        .last_response_status = app->last_response_status,
        .queued_inputs = app->input_count,
        .notification = app->notification,
        .quit_requested = app->quit_requested,
    };
}
