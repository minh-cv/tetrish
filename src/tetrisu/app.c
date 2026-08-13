#include "app.h"

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
    const void* payload,
    size_t payload_len
) {
    if (effects->count == APP_EFFECT_LIST_CAPACITY) {
        return -1;
    }
    AppEffect effect = {
        .type = type,
        .method = method,
        .path = path,
        .content_type = content_type,
    };
    owned_bytes_init(&effect.payload);
    if (owned_bytes_copy(&effect.payload, payload, payload_len) == -1) {
        return -1;
    }
    effects->items[effects->count++] = effect;
    return 0;
}

static int push_simple_effect(AppEffectList* effects, AppEffectType type) {
    return push_effect(effects, type, NULL, NULL, NULL, NULL, 0);
}

static int push_request_effect(
    AppState* app,
    AppEffectList* effects,
    const char* method,
    const void* payload,
    size_t payload_len
) {
    if (app->connection != APP_CONNECTION_READY) {
        set_notification(app, "Not connected");
        return 0;
    }
    if (app->request != APP_REQUEST_IDLE) {
        set_notification(app, "A request is already pending");
        return 0;
    }
    app->request = APP_REQUEST_SUBMITTING;
    app->view_dirty = true;
    return push_effect(
        effects,
        APP_EFFECT_NET_SEND,
        method,
        "",
        "text/plain",
        payload,
        payload_len
    );
}

void app_init(AppState* app) {
    memset(app, 0, sizeof(*app));
    app->connection = APP_CONNECTION_DISCONNECTED;
    app->request = APP_REQUEST_IDLE;
    owned_bytes_init(&app->remote_state);
    owned_bytes_init(&app->last_message);
    set_notification(app, "Starting tetrisu");
}

void app_free(AppState* app) {
    owned_bytes_free(&app->remote_state);
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

static int reduce_command(
    AppState* app,
    const ParsedCommand* command,
    AppEffectList* effects
) {
    switch (command->type) {
    case COMMAND_HELP:
        set_notification(
            app,
            "Commands: htttp <text>, set-name <name>, whoami, reconnect, quit"
        );
        return 0;
    case COMMAND_QUIT:
        app->quit_requested = true;
        app->view_dirty = true;
        return push_simple_effect(effects, APP_EFFECT_QUIT);
    case COMMAND_RECONNECT:
        app->connection = APP_CONNECTION_CONNECTING;
        app->request = APP_REQUEST_IDLE;
        app->view_dirty = true;
        if (push_simple_effect(effects, APP_EFFECT_NET_DISCONNECT) == -1) {
            return -1;
        }
        return push_simple_effect(effects, APP_EFFECT_NET_CONNECT);
    case COMMAND_DISCONNECT:
        return push_simple_effect(effects, APP_EFFECT_NET_DISCONNECT);
    case COMMAND_SEND_RAW:
        return push_request_effect(
            app,
            effects,
            "HTTTP",
            command->argument,
            command->argument_len
        );
    case COMMAND_SET_NAME:
        return push_request_effect(
            app,
            effects,
            "SET_PLAYER_NAME",
            command->argument,
            command->argument_len
        );
    case COMMAND_WHOAMI:
        return push_request_effect(app, effects, "WHOAMI", NULL, 0);
    case COMMAND_UNSUPPORTED:
        set_notification(app, "Unsupported command");
        return 0;
    }
    return -1;
}

static int reduce_network(AppState* app, const NetEvent* event) {
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
        set_notification(app, "Connected");
        break;
    case NET_EVENT_SEND_ACCEPTED:
        app->request = APP_REQUEST_PENDING;
        set_notification(app, "Request pending");
        break;
    case NET_EVENT_REPLY:
    case NET_EVENT_ECHO:
        if (replace_bytes(&app->last_message, &event->payload) == -1) {
            return -1;
        }
        app->request = APP_REQUEST_IDLE;
        set_notification(app, event->type == NET_EVENT_ECHO ? "Echo received" : "Reply received");
        break;
    case NET_EVENT_STATE_PUSH:
        if (replace_bytes(&app->remote_state, &event->payload) == -1) {
            return -1;
        }
        set_notification(app, "STATE push received");
        break;
    case NET_EVENT_DISCONNECTED:
        app->connection = APP_CONNECTION_DISCONNECTED;
        app->request = APP_REQUEST_IDLE;
        set_notification(app, "Disconnected");
        break;
    case NET_EVENT_ERROR:
        app->connection = APP_CONNECTION_DISCONNECTED;
        app->request = APP_REQUEST_IDLE;
        set_notification(app, event->error.detail != NULL ? event->error.detail : "Network error");
        break;
    }
    app->view_dirty = true;
    return 0;
}

int app_reduce(AppState* app, const AppEvent* event, AppEffectList* effects) {
    switch (event->type) {
    case APP_EVENT_START:
        app->connection = APP_CONNECTION_CONNECTING;
        app->view_dirty = true;
        return push_simple_effect(effects, APP_EFFECT_NET_CONNECT);
    case APP_EVENT_COMMAND_SUBMITTED:
        return reduce_command(app, event->data.command, effects);
    case APP_EVENT_NETWORK:
        return reduce_network(app, event->data.network);
    }
    return -1;
}

void app_build_view(const AppState* app, AppView* view) {
    *view = (AppView){
        .connection = app->connection,
        .request = app->request,
        .remote_state = &app->remote_state,
        .last_message = &app->last_message,
        .notification = app->notification,
        .quit_requested = app->quit_requested,
    };
}
