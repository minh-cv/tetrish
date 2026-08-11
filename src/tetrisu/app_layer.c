#include "app_layer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRANSCRIPT_LINE_MAX 1024

static void transcript_drop_oldest(ViewModel* view) {
    TranscriptLine* const line = TranscriptQueue_front(&view->transcript);
    if (line == NULL) {
        return;
    }
    free(line->text);
    TranscriptQueue_pop_front(&view->transcript);
}

/*
    Takes ownership of `text` on every path, so a caller that formatted a line
    never has to think about the queue being full.
*/
static void transcript_push_owned(ViewModel* view, char* text) {
    if (text == NULL) {
        return;
    }
    // the oldest line is the one the user is least likely to still need
    if (TranscriptQueue_size(&view->transcript) == TranscriptQueue_capacity(&view->transcript)) {
        transcript_drop_oldest(view);
    }
    const TranscriptLine line = {text};
    if (TranscriptQueue_push_back(&view->transcript, &line) == -1) {
        free(text);
        return;
    }
    view->dirty = true;
}

static void note_v(ViewModel* view, const char* format, va_list args) {
    char scratch[TRANSCRIPT_LINE_MAX];
    const int written = vsnprintf(scratch, sizeof(scratch), format, args);
    if (written < 0) {
        return;
    }

    const size_t len = (size_t)written < sizeof(scratch) ? (size_t)written : sizeof(scratch) - 1;
    char* const text = malloc(len + 1);
    if (text == NULL) {
        return;
    }
    memcpy(text, scratch, len);
    text[len] = '\0';
    transcript_push_owned(view, text);
}

static void note(ViewModel* view, const char* format, ...) __attribute__((format(printf, 2, 3)));

static void note(ViewModel* view, const char* format, ...) {
    va_list args;
    va_start(args, format);
    note_v(view, format, args);
    va_end(args);
}

void AppData_note(AppData* data, const char* format, ...) {
    va_list args;
    va_start(args, format);
    note_v(&data->view, format, args);
    va_end(args);
}

int AppData_init(AppData* data, size_t queue_capacity) {
    memset(data, 0, sizeof(*data));

    if (HtttpOutboundMessageQueue_init(&data->request_q, queue_capacity) == -1) {
        return -1;
    }
    if (PendingRequestQueue_init(&data->pending, queue_capacity) == -1) {
        HtttpOutboundMessageQueue_free(&data->request_q);
        return -1;
    }
    if (TranscriptQueue_init(&data->view.transcript, queue_capacity) == -1) {
        PendingRequestQueue_free(&data->pending);
        HtttpOutboundMessageQueue_free(&data->request_q);
        return -1;
    }

    data->view.player_id = -1;
    data->view.connected = true;
    data->view.mode = MODE_SHELL;
    data->requested_mode = MODE_SHELL;
    snprintf(data->view.player_name, sizeof(data->view.player_name), "%s", "(unnamed)");
    return 0;
}

void AppData_free(AppData* data) {
    while (!TranscriptQueue_empty(&data->view.transcript)) {
        transcript_drop_oldest(&data->view);
    }
    TranscriptQueue_free(&data->view.transcript);
    PendingRequestQueue_free(&data->pending);

    const size_t count = HtttpOutboundMessageQueue_size(&data->request_q);
    for (size_t i = 0; i < count; i++) {
        HtttpOutboundMessage* const message = HtttpOutboundMessageQueue_front(&data->request_q);
        htttp_message_free(&message->message, &message->ownership);
        HtttpOutboundMessageQueue_pop_front(&data->request_q);
    }
    HtttpOutboundMessageQueue_free(&data->request_q);
}

void AppData_reset(AppData* data) {
    (void)data;
}

/*
    Every outbound message is built here, so the headers the spec requires on a
    client request are attached in exactly one place.
*/
static int make_request(const AppData* data, const char* method, const char* path,
                        const char* body, size_t body_len, HtttpOutboundMessage* out) {
    memset(out, 0, sizeof(*out));

    char* const path_copy = strdup(path);
    if (path_copy == NULL) {
        return -1;
    }

    char scratch[32];
    int written = snprintf(scratch, sizeof(scratch), "%ld", data->view.player_id);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        free(path_copy);
        return -1;
    }
    char* const player_id = strdup(scratch);
    if (player_id == NULL) {
        free(path_copy);
        return -1;
    }

    char* body_copy = NULL;
    char* content_length = NULL;
    if (body_len != 0) {
        body_copy = malloc(body_len);
        written = snprintf(scratch, sizeof(scratch), "%zu", body_len);
        if (body_copy == NULL || written < 0 || (size_t)written >= sizeof(scratch)) {
            free(body_copy);
            free(player_id);
            free(path_copy);
            return -1;
        }
        memcpy(body_copy, body, body_len);
        content_length = strdup(scratch);
        if (content_length == NULL) {
            free(body_copy);
            free(player_id);
            free(path_copy);
            return -1;
        }
    }

    out->message.is_request = true;
    out->message.request.method = method;   // a literal or an argv token
    out->message.request.path = path_copy;
    out->ownership.is_path_owned = true;

    size_t header_count = 0;
    out->message.request.header[header_count].key = "Player-Id";
    out->message.request.header[header_count].value = player_id;
    out->ownership.is_value_owned[header_count] = true;
    header_count++;

    if (body_len != 0) {
        out->message.request.header[header_count].key = "Content-Length";
        out->message.request.header[header_count].value = content_length;
        out->ownership.is_value_owned[header_count] = true;
        header_count++;

        out->message.request.header[header_count].key = "Content-Type";
        out->message.request.header[header_count].value = "application/tetris-command";
        header_count++;

        out->message.request.body = (const unsigned char*)body_copy;
        out->message.request.body_len = body_len;
        out->ownership.is_body_owned = true;
    }
    out->message.request.header_count = header_count;
    return 0;
}

/*
    The method string may be an argv token owned by the input event, which is
    freed at the end of the tick, so a request that outlives the tick must not
    point at one. Copying it into the message would need an ownership bit
    htttp does not have for the method, so instead the request is guaranteed to
    be serialized in the same tick: HtttpData_serialize runs after this layer.
*/
static int send_request(AppData* data, HtttpOutboundMessageQueue* m_request_q,
                        const char* method, const char* path,
                        const char* body, size_t body_len, PendingKind kind, const char* name) {
    HtttpOutboundMessage message;
    if (make_request(data, method, path, body, body_len, &message) == -1) {
        return -1;
    }
    if (HtttpOutboundMessageQueue_push_back(m_request_q, &message) == -1) {
        htttp_message_free(&message.message, &message.ownership);
        return -1;
    }

    PendingRequest pending;
    memset(&pending, 0, sizeof(pending));
    pending.kind = kind;
    if (name != NULL) {
        snprintf(pending.name, sizeof(pending.name), "%s", name);
    }
    if (PendingRequestQueue_push_back(&data->pending, &pending) == -1) {
        // the response would be misattributed to the next request, so this is
        // worse than not sending at all
        return -1;
    }
    return 0;
}

/* ---- command table ---- */

typedef int (*CommandHandler)(AppData*, size_t argc, char* const argv[],
                              HtttpOutboundMessageQueue* m_request_q);

typedef struct {
    const char* name;
    size_t min_args;   // including the command word
    size_t max_args;
    const char* usage;
    CommandHandler handler;
} CommandEntry;

static int cmd_htttp(AppData* data, size_t argc, char* const argv[],
                     HtttpOutboundMessageQueue* m_request_q) {
    const char* const method = argv[1];
    const char* const path = argc >= 3 ? argv[2] : "/";
    const char* const body = argc >= 4 ? argv[3] : NULL;
    const size_t body_len = body == NULL ? 0 : strlen(body);
    return send_request(data, m_request_q, method, path, body, body_len, PENDING_GENERIC, NULL);
}

static int cmd_set_name(AppData* data, size_t argc, char* const argv[],
                        HtttpOutboundMessageQueue* m_request_q) {
    (void)argc;
    return send_request(data, m_request_q, "SET_PLAYER_NAME", "/player", argv[1], strlen(argv[1]),
                        PENDING_SET_NAME, argv[1]);
}

static int cmd_whoami(AppData* data, size_t argc, char* const argv[],
                      HtttpOutboundMessageQueue* m_request_q) {
    (void)argc;
    (void)argv;
    return send_request(data, m_request_q, "WHOAMI", "/player", NULL, 0, PENDING_WHOAMI, NULL);
}

/*
    A room of one. The client picks the code so two players on the same daemon
    cannot collide by accident; the request is the same JOIN a multiplayer
    room would use, which is what keeps singleplayer from being a second code
    path through the server.
*/
static void make_room_code(char* out, size_t out_size) {
    static const char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    size_t i = 0;
    for (; i + 1 < out_size && i < 6; i++) {
        out[i] = ALPHABET[(size_t)rand() % (sizeof(ALPHABET) - 1)];
    }
    out[i] = '\0';
}

static int join_room(AppData* data, HtttpOutboundMessageQueue* m_request_q, const char* code) {
    char path[CLIENT_ROOM_MAX + 8];
    if (snprintf(path, sizeof(path), "/room/%s", code) < 0) {
        return -1;
    }
    snprintf(data->view.room, sizeof(data->view.room), "%s", code);
    return send_request(data, m_request_q, "JOIN", path, NULL, 0, PENDING_JOIN, NULL);
}

static int cmd_singleplayer(AppData* data, size_t argc, char* const argv[],
                            HtttpOutboundMessageQueue* m_request_q) {
    if (data->view.mode != MODE_SHELL) {
        return 0;
    }

    char code[CLIENT_ROOM_MAX];
    if (argc >= 2) {
        snprintf(code, sizeof(code), "%s", argv[1]);
    }
    else {
        make_room_code(code, sizeof(code));
    }
    return join_room(data, m_request_q, code);
}

static int cmd_quit(AppData* data, size_t argc, char* const argv[],
                    HtttpOutboundMessageQueue* m_request_q) {
    (void)argc;
    (void)argv;
    (void)m_request_q;
    data->quit = true;
    return 0;
}

static int cmd_help(AppData* data, size_t argc, char* const argv[],
                    HtttpOutboundMessageQueue* m_request_q);

static const CommandEntry COMMAND_TABLE[] = {
    {"htttp", 2, 4, "htttp <METHOD> [PATH] [BODY]", cmd_htttp},
    {"set-name", 2, 2, "set-name <name>", cmd_set_name},
    {"whoami", 1, 1, "whoami", cmd_whoami},
    {"singleplayer", 1, 2, "singleplayer [room]", cmd_singleplayer},
    {"help", 1, 1, "help", cmd_help},
    {"quit", 1, 1, "quit", cmd_quit},
};

static const size_t COMMAND_TABLE_COUNT = sizeof(COMMAND_TABLE) / sizeof(COMMAND_TABLE[0]);

static int cmd_help(AppData* data, size_t argc, char* const argv[],
                    HtttpOutboundMessageQueue* m_request_q) {
    (void)argc;
    (void)argv;
    (void)m_request_q;
    note(&data->view, "commands:");
    for (size_t i = 0; i < COMMAND_TABLE_COUNT; i++) {
        note(&data->view, "  %s", COMMAND_TABLE[i].usage);
    }
    return 0;
}

static void handle_command(AppData* data, const InputEvent* event,
                           HtttpOutboundMessageQueue* m_request_q, ClientFault* fault) {
    const char* const name = event->command.argv[0];

    for (size_t i = 0; i < COMMAND_TABLE_COUNT; i++) {
        if (strcmp(name, COMMAND_TABLE[i].name) != 0) {
            continue;
        }
        if (event->command.argc < COMMAND_TABLE[i].min_args ||
            event->command.argc > COMMAND_TABLE[i].max_args) {
            note(&data->view, "usage: %s", COMMAND_TABLE[i].usage);
            return;
        }
        if (COMMAND_TABLE[i].handler(data, event->command.argc, event->command.argv,
                                     m_request_q) == -1) {
            *fault = FAULT_LOCAL;
        }
        return;
    }

    note(&data->view, "unknown command: %s (try `help`)", name);
}

/* ---- inbound routing ---- */

/*
    Reads `key=value` out of a line-oriented body without copying it first.
*/
static bool body_field(const unsigned char* body, size_t body_len, const char* key,
                       char* out, size_t out_size) {
    const size_t key_len = strlen(key);
    size_t i = 0;
    while (i < body_len) {
        size_t end = i;
        while (end < body_len && body[end] != '\n') {
            end++;
        }
        if (end - i > key_len && memcmp(body + i, key, key_len) == 0) {
            const size_t value_len = end - i - key_len;
            const size_t copied = value_len < out_size - 1 ? value_len : out_size - 1;
            memcpy(out, body + i + key_len, copied);
            out[copied] = '\0';
            return true;
        }
        i = end + 1;
    }
    return false;
}

static void adopt_player_id(AppData* data, const HtttpMessage* message) {
    const char* const value = htttp_get_header(message, "Player-Id");
    if (value == NULL) {
        return;
    }
    char* end;
    const long id = strtol(value, &end, 10);
    if (*end != '\0' || id < 0 || id == data->view.player_id) {
        return;
    }
    data->view.player_id = id;
    data->view.dirty = true;
}

static void handle_response(AppData* data, const HtttpMessage* message,
                            HtttpOutboundMessageQueue* m_request_q) {
    adopt_player_id(data, message);

    const HtttpResponse* const response = &message->response;
    const bool ok = response->status >= 200 && response->status < 300;

    PendingRequest* const pending = PendingRequestQueue_front(&data->pending);
    if (pending == NULL) {
        note(&data->view, "unsolicited response %d %s", response->status, response->reason);
        return;
    }
    const PendingRequest request = *pending;
    PendingRequestQueue_pop_front(&data->pending);

    switch (request.kind) {
    case PENDING_JOIN:
        if (ok) {
            char path[CLIENT_ROOM_MAX + 8];
            snprintf(path, sizeof(path), "/room/%s", data->view.room);
            if (send_request(data, m_request_q, "START", path, NULL, 0, PENDING_START, NULL) == -1) {
                note(&data->view, "cannot ask the server to start the room");
            }
            return;
        }
        data->view.room[0] = '\0';
        break;
    case PENDING_START:
        if (ok) {
            // the switch itself happens at tick end, once the UI has drawn the
            // last shell frame and the input discipline can change with it
            data->requested_mode = MODE_GAME;
            data->view.game_valid = false;
            return;
        }
        data->view.room[0] = '\0';
        break;
    case PENDING_LEAVE:
        data->view.room[0] = '\0';
        data->requested_mode = MODE_SHELL;
        break;
    case PENDING_INPUT:
        // an input the server refused is worth saying once; an accepted one is
        // 60 lines a second of nothing
        if (!ok) {
            note(&data->view, "input refused: %d %s", response->status, response->reason);
        }
        return;
    case PENDING_SET_NAME:
        if (ok) {
            snprintf(data->view.player_name, sizeof(data->view.player_name), "%s", request.name);
        }
        break;
    case PENDING_WHOAMI:
        if (ok) {
            body_field(response->body, response->body_len, "name=",
                       data->view.player_name, sizeof(data->view.player_name));
        }
        break;
    case PENDING_GENERIC:
        break;
    }

    if (response->body_len == 0) {
        note(&data->view, "%d %s", response->status, response->reason);
    }
    else {
        note(&data->view, "%d %s: %.*s", response->status, response->reason,
             (int)response->body_len, (const char*)response->body);
    }
}

/*
    STATE is the only message the server originates, so the push table is one
    branch for now; it is written as a lookup so a second push kind is a row
    rather than a rewrite.
*/
static void handle_push(AppData* data, const HtttpMessage* message) {
    adopt_player_id(data, message);

    if (strcmp(message->request.method, "STATE") != 0) {
        note(&data->view, "unhandled push %s %s", message->request.method, message->request.path);
        return;
    }

    if (game_view_decode(&data->view.game, (const char*)message->request.body,
                         message->request.body_len) == -1) {
        note(&data->view, "unreadable state push");
        return;
    }
    data->view.game_valid = true;
    data->view.game_seq++;
    data->view.dirty = true;

    if (data->view.mode == MODE_SHELL && data->view.game.status == GAME_STATUS_RUNNING) {
        return;
    }
}

static void route_inbound(AppData* data, const HtttpParsedMessageQueue* m_parsed_q,
                          HtttpOutboundMessageQueue* m_request_q, ClientFault* fault) {
    const size_t count = HtttpParsedMessageQueue_size(m_parsed_q);
    for (size_t i = 0; i < count; i++) {
        const HtttpParsedMessage* const parsed = HtttpParsedMessageQueue_at(m_parsed_q, i);

        switch (parsed->status) {
        case FRAME_OK:
            if (parsed->message.is_request) {
                handle_push(data, &parsed->message);
            }
            else {
                handle_response(data, &parsed->message, m_request_q);
            }
            break;
        case FRAME_DECRYPT_ERROR:
            // a client that cannot decrypt has lost session sync and has
            // nothing useful left to say
            note(&data->view, "cannot decrypt a frame from the server");
            *fault = FAULT_TRANSPORT;
            return;
        case FRAME_PAYLOAD_TOO_LARGE:
            note(&data->view, "the server sent an oversized frame; ignored");
            break;
        case FRAME_HTTTP_PARSE_ERROR:
            note(&data->view, "the server sent a malformed message; ignored");
            break;
        }
    }
}

/*
    Key bindings. Arrows and the WASD-ish cluster both work, because a terminal
    that swallows arrow keys is common enough that a demo should not depend on
    them.
*/
static bool bind_key(const InputEvent* event, PlayerInputKey* out) {
    switch (event->key.key) {
    case TUI_KEY_LEFT: *out = PLAYER_INPUT_KEY_MOVE_LEFT; return true;
    case TUI_KEY_RIGHT: *out = PLAYER_INPUT_KEY_MOVE_RIGHT; return true;
    case TUI_KEY_DOWN: *out = PLAYER_INPUT_KEY_SOFT_DROP; return true;
    case TUI_KEY_UP: *out = PLAYER_INPUT_KEY_ROTATE_RIGHT; return true;
    case TUI_KEY_SPACE: *out = PLAYER_INPUT_KEY_LOCK_DOWN; return true;
    default: break;
    }

    switch (event->key.ch) {
    case 'a': case 'A': *out = PLAYER_INPUT_KEY_MOVE_LEFT; return true;
    case 'd': case 'D': *out = PLAYER_INPUT_KEY_MOVE_RIGHT; return true;
    case 's': case 'S': *out = PLAYER_INPUT_KEY_SOFT_DROP; return true;
    case 'w': case 'W': *out = PLAYER_INPUT_KEY_LOCK_DOWN; return true;
    case 'x': case 'X': *out = PLAYER_INPUT_KEY_ROTATE_RIGHT; return true;
    case 'z': case 'Z': *out = PLAYER_INPUT_KEY_ROTATE_LEFT; return true;
    case 'c': case 'C': *out = PLAYER_INPUT_KEY_HOLD; return true;
    default: return false;
    }
}

static int leave_room(AppData* data, HtttpOutboundMessageQueue* m_request_q) {
    char path[CLIENT_ROOM_MAX + 8];
    if (snprintf(path, sizeof(path), "/room/%s", data->view.room) < 0) {
        return -1;
    }
    return send_request(data, m_request_q, "LEAVE", path, NULL, 0, PENDING_LEAVE, NULL);
}

static void handle_game_key(AppData* data, const InputEvent* event,
                            HtttpOutboundMessageQueue* m_request_q, ClientFault* fault) {
    if (event->key.ch == 'q' || event->key.ch == 'Q' || event->key.key == TUI_KEY_ESCAPE) {
        if (leave_room(data, m_request_q) == -1) {
            *fault = FAULT_LOCAL;
        }
        return;
    }

    PlayerInputKey key;
    if (bind_key(event, &key)) {
        // latched, not sent: the frame tick is what turns presses into
        // requests, which bounds the request rate to the frame rate and
        // matches the per-frame key array the server applies
        data->input_latch[key] = true;
    }
}

/*
    One request per latched key. The bodies are the words the spec fixes for
    each method, so the mapping is a table rather than a chain of ifs.
*/
static int flush_input_latch(AppData* data, HtttpOutboundMessageQueue* m_request_q) {
    static const struct {
        PlayerInputKey key;
        const char* method;
        const char* body;
    } BINDINGS[] = {
        {PLAYER_INPUT_KEY_MOVE_LEFT, "MOVE", "LEFT"},
        {PLAYER_INPUT_KEY_MOVE_RIGHT, "MOVE", "RIGHT"},
        {PLAYER_INPUT_KEY_ROTATE_RIGHT, "ROTATE", "CW"},
        {PLAYER_INPUT_KEY_ROTATE_LEFT, "ROTATE", "CCW"},
        {PLAYER_INPUT_KEY_SOFT_DROP, "DROP", "SOFT"},
        {PLAYER_INPUT_KEY_LOCK_DOWN, "DROP", "HARD"},
        {PLAYER_INPUT_KEY_HOLD, "HOLD", NULL},
    };
    static const size_t BINDING_COUNT = sizeof(BINDINGS) / sizeof(BINDINGS[0]);

    char path[CLIENT_ROOM_MAX + 32];
    if (snprintf(path, sizeof(path), "/room/%s/player/%ld", data->view.room,
                 data->view.player_id) < 0) {
        return -1;
    }

    for (size_t i = 0; i < BINDING_COUNT; i++) {
        if (!data->input_latch[BINDINGS[i].key]) {
            continue;
        }
        data->input_latch[BINDINGS[i].key] = false;

        const char* const body = BINDINGS[i].body;
        if (send_request(data, m_request_q, BINDINGS[i].method, path, body,
                         body == NULL ? 0 : strlen(body), PENDING_INPUT, NULL) == -1) {
            return -1;
        }
    }
    return 0;
}

static void handle_input(AppData* data, const InputEventQueue* m_event_q,
                         HtttpOutboundMessageQueue* m_request_q, ClientFault* fault) {
    const size_t count = InputEventQueue_size(m_event_q);
    for (size_t i = 0; i < count; i++) {
        const InputEvent* const event = InputEventQueue_at(m_event_q, i);

        switch (event->type) {
        case INPUT_EVENT_COMMAND:
            if (data->view.mode == MODE_SHELL) {
                handle_command(data, event, m_request_q, fault);
            }
            break;
        case INPUT_EVENT_KEY:
            if (data->view.mode == MODE_GAME) {
                handle_game_key(data, event, m_request_q, fault);
            }
            break;
        case INPUT_EVENT_DIAGNOSTIC:
            note(&data->view, "%s", event->diagnostic);
            break;
        case INPUT_EVENT_EOF:
            data->quit = true;
            break;
        }
    }
}

void AppData_step(AppData* data, const InputEventQueue* m_event_q,
                  const HtttpParsedMessageQueue* m_parsed_q,
                  HtttpOutboundMessageQueue* m_request_q,
                  bool frame_tick, ClientFault* fault) {
    if (*fault != FAULT_NONE) {
        return;
    }

    route_inbound(data, m_parsed_q, m_request_q, fault);
    if (*fault != FAULT_NONE) {
        return;
    }
    handle_input(data, m_event_q, m_request_q, fault);
    if (*fault != FAULT_NONE) {
        return;
    }

    if (frame_tick && data->view.mode == MODE_GAME &&
        flush_input_latch(data, m_request_q) == -1) {
        *fault = FAULT_LOCAL;
    }
}
