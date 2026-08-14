#include "app.h"
#include "command/parser.h"
#include "command/router.h"
#include "game/request.h"
#include "net/htttp_codec.h"
#include "net/inbound_policy.h"

#include "htttp.h"
#include "proto.h"
#include "tetrisbrain/control.h"
#include "tetrisbrain/state.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* line;
    GameIntentType intent;
} CommandCase;

static ParsedCommand parse_and_route(const char* input) {
    CommandArgv argv = {0};
    assert(command_argv_parse(input, strlen(input), &argv) == COMMAND_PARSE_OK);
    ParsedCommand command = {0};
    assert(command_route(&argv, &command) == COMMAND_ROUTE_OK);
    command_argv_free(&argv);
    return command;
}

static CommandRouteResult route_result(const char* input) {
    CommandArgv argv = {0};
    assert(command_argv_parse(input, strlen(input), &argv) == COMMAND_PARSE_OK);
    ParsedCommand command = {0};
    const CommandRouteResult result = command_route(&argv, &command);
    parsed_command_free(&command);
    command_argv_free(&argv);
    return result;
}

static void test_command_parser_and_router(void) {
    static const CommandCase cases[] = {
        {"create", GAME_INTENT_CREATE},
        {"join", GAME_INTENT_JOIN},
        {"start", GAME_INTENT_START},
        {"move left", GAME_INTENT_MOVE_LEFT},
        {"move right", GAME_INTENT_MOVE_RIGHT},
        {"rotate cw", GAME_INTENT_ROTATE_CW},
        {"rotate ccw", GAME_INTENT_ROTATE_CCW},
        {"drop soft", GAME_INTENT_DROP_SOFT},
        {"drop hard", GAME_INTENT_DROP_HARD},
        {"hold", GAME_INTENT_HOLD},
        {"leave", GAME_INTENT_LEAVE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        ParsedCommand command = parse_and_route(cases[i].line);
        assert(command.type == COMMAND_GAME);
        assert(command.game_intent == cases[i].intent);
        parsed_command_free(&command);
    }

    assert(route_result("move") == COMMAND_ROUTE_MISSING_ARGUMENT);
    assert(route_result("move up") == COMMAND_ROUTE_INVALID_ARGUMENT);
    assert(route_result("hold now") == COMMAND_ROUTE_TOO_MANY_ARGUMENTS);
    assert(route_result("drop hard now") == COMMAND_ROUTE_TOO_MANY_ARGUMENTS);

    static const char escaped[] = "htttp \"line\\n\\\"quoted\\\"\"";
    CommandArgv argv = {0};
    assert(command_argv_parse(escaped, strlen(escaped), &argv) == COMMAND_PARSE_OK);
    assert(strcmp(argv.argv[1], "line\n\"quoted\"") == 0);
    command_argv_free(&argv);
    static const char unterminated[] = "htttp 'open";
    assert(command_argv_parse(
        unterminated, sizeof(unterminated) - 1, &argv
    ) == COMMAND_PARSE_UNTERMINATED_QUOTE);
}

static void assert_request(
    GameIntentType intent,
    const char* method,
    const char* body,
    ClientRequestCompletion completion
) {
    ClientRequest request;
    assert(game_request_from_intent(intent, &request) == 0);
    assert(strcmp(request.method, method) == 0);
    assert(strcmp(request.path, "/") == 0);
    assert(strcmp(request.content_type, "application/tetris-command") == 0);
    assert(request.completion == completion);
    const size_t body_len = body == NULL ? 0 : strlen(body);
    assert(request.body_len == body_len);
    assert(body_len == 0 || memcmp(request.body, body, body_len) == 0);

    OwnedBytes encoded = {0};
    assert(htttp_codec_encode_request(&request, &encoded) == 0);
    OwnedHtttpMessage decoded = {0};
    assert(htttp_codec_decode_owned(&encoded, &decoded) == 0);
    assert(decoded.view.is_request);
    assert(strcmp(decoded.view.request.method, method) == 0);
    const OwnedBytes decoded_body = htttp_codec_borrow_body(&decoded);
    assert(decoded_body.len == body_len);
    assert(body_len == 0 || memcmp(decoded_body.ptr, body, body_len) == 0);
    owned_htttp_message_free(&decoded);
}

static void test_game_request_mapping(void) {
    assert_request(GAME_INTENT_CREATE, "CREATE", NULL, CLIENT_REQUEST_EXPECT_REPLY);
    assert_request(GAME_INTENT_JOIN, "JOIN", NULL, CLIENT_REQUEST_EXPECT_REPLY);
    assert_request(GAME_INTENT_START, "START", NULL, CLIENT_REQUEST_EXPECT_REPLY);
    assert_request(GAME_INTENT_MOVE_LEFT, "MOVE", "LEFT", CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_MOVE_RIGHT, "MOVE", "RIGHT", CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_ROTATE_CW, "ROTATE", "CW", CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_ROTATE_CCW, "ROTATE", "CCW", CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_DROP_SOFT, "DROP", "SOFT", CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_DROP_HARD, "DROP", "HARD", CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_HOLD, "HOLD", NULL, CLIENT_REQUEST_COMPLETE_ON_SEND);
    assert_request(GAME_INTENT_LEAVE, "LEAVE", NULL, CLIENT_REQUEST_EXPECT_REPLY);
}

static OwnedHtttpMessage make_request(
    const char* method,
    const char* path,
    const char* content_type,
    const unsigned char* body,
    size_t body_len
) {
    char body_length[32];
    (void)snprintf(body_length, sizeof(body_length), "%zu", body_len);
    const HtttpMessage source = {
        .request = {
            .method = method,
            .path = path,
            .header = {
                {"Content-Length", body_length},
                {"Content-Type", content_type},
            },
            .header_count = 2,
            .body = body,
            .body_len = body_len,
        },
        .is_request = true,
    };
    OwnedBytes bytes = {0};
    bytes.ptr = htttp_serialize(&source, &bytes.len);
    assert(bytes.ptr != NULL);
    OwnedHtttpMessage decoded = {0};
    assert(htttp_codec_decode_owned(&bytes, &decoded) == 0);
    return decoded;
}

static OwnedHtttpMessage make_response(HtttpStatus status, const char* body) {
    const size_t body_len = body == NULL ? 0 : strlen(body);
    char body_length[32];
    (void)snprintf(body_length, sizeof(body_length), "%zu", body_len);
    const HtttpMessage source = {
        .response = {
            .status = status,
            .reason = "Bad Request",
            .header = {{"Content-Length", body_length}},
            .header_count = 1,
            .body = (const unsigned char*)body,
            .body_len = body_len,
        },
        .is_request = false,
    };
    OwnedBytes bytes = {0};
    bytes.ptr = htttp_serialize(&source, &bytes.len);
    assert(bytes.ptr != NULL);
    OwnedHtttpMessage decoded = {0};
    assert(htttp_codec_decode_owned(&bytes, &decoded) == 0);
    return decoded;
}

static ProtoStateRequest make_state(bool active) {
    State game = init_state(7);
    assert(!apply_spawn(&game));
    return (ProtoStateRequest){
        .board_state = game.board_state,
        .combo_counter = game.combo_counter,
        .hold_state = game.hold_state,
        .bag_state = game.bag_state,
        .garbage_balance = game.garbage_balance,
        .back_to_back_count = game.back_to_back_count,
        .game_score = game.score,
        .is_game_active = active,
    };
}

static void test_state_codec_and_inbound_policy(void) {
    const ProtoStateRequest expected = make_state(true);
    unsigned char body[PROTO_STATE_REQUEST_BODY_LEN];
    proto_encode_state_request(&expected, body);
    ProtoStateRequest parsed;
    memset(&parsed, 0, sizeof(parsed));
    assert(proto_parse_state_request(body, sizeof(body), &parsed) == 0);
    assert(parsed.is_game_active);
    assert(parsed.board_state.tetromino.type == expected.board_state.tetromino.type);
    assert(parsed.bag_state.bag1_offset == expected.bag_state.bag1_offset);

    OwnedHtttpMessage state = make_request(
        "STATE", "/room/8", "application/tetris-state", body, sizeof(body)
    );
    assert(net_inbound_classify(&state, false, false) == NET_INBOUND_STATE_PUSH);
    assert(net_inbound_classify(&state, true, false) == NET_INBOUND_STATE_PUSH);
    owned_htttp_message_free(&state);

    OwnedHtttpMessage wrong_state = make_request(
        "STATE", "/", "application/tetris-state", body, sizeof(body)
    );
    assert(net_inbound_classify(&wrong_state, false, false) == NET_INBOUND_REJECT);
    owned_htttp_message_free(&wrong_state);

    OwnedHtttpMessage wrong_type = make_request(
        "STATE", "/room/8", "application/octet-stream", body, sizeof(body)
    );
    assert(net_inbound_classify(&wrong_type, false, false) == NET_INBOUND_REJECT);
    owned_htttp_message_free(&wrong_type);

    static const unsigned char x[] = "x";
    OwnedHtttpMessage unknown = make_request("PING", "/", "text/plain", x, 1);
    assert(net_inbound_classify(&unknown, false, false) == NET_INBOUND_REJECT);
    assert(net_inbound_classify(&unknown, true, true) == NET_INBOUND_LEGACY_ECHO);
    owned_htttp_message_free(&unknown);

    /*
        The daemon answers a frame it could not parse before it knows which
        method the frame carried, and gameplay inputs complete on send, so
        this arrives with nothing outstanding. It must not be confused with
        the unknown server request above, which stays fatal.
    */
    OwnedHtttpMessage late = make_response(HTTTP_STATUS_BAD_REQUEST, "Cannot parse request");
    assert(net_inbound_classify(&late, false, false) == NET_INBOUND_UNSOLICITED_REPLY);
    assert(net_inbound_classify(&late, true, false) == NET_INBOUND_REPLY);
    owned_htttp_message_free(&late);
}

static void reduce_network(
    AppState* app,
    const NetEvent* network,
    AppEffectList* effects
) {
    app_effect_list_init(effects);
    const AppEvent event = {.type = APP_EVENT_NETWORK, .data.network = network};
    assert(app_reduce(app, &event, effects) == 0);
}

static void reduce_command(
    AppState* app,
    GameIntentType intent,
    AppEffectList* effects
) {
    const ParsedCommand command = {
        .type = COMMAND_GAME,
        .game_intent = intent,
    };
    const AppEvent event = {.type = APP_EVENT_COMMAND_SUBMITTED, .data.command = &command};
    app_effect_list_init(effects);
    assert(app_reduce(app, &event, effects) == 0);
}

static void connect_app(AppState* app) {
    AppEffectList effects;
    const NetEvent connected = {.type = NET_EVENT_CONNECTED};
    reduce_network(app, &connected, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
}

static void complete_control(
    AppState* app,
    GameIntentType intent,
    int status
) {
    AppEffectList effects;
    reduce_command(app, intent, &effects);
    assert(effects.count == 1);
    assert(effects.items[0].completion == CLIENT_REQUEST_EXPECT_REPLY);
    app_effect_list_free(&effects);

    const NetEvent accepted = {
        .type = NET_EVENT_SEND_ACCEPTED,
        .completion = CLIENT_REQUEST_EXPECT_REPLY,
    };
    reduce_network(app, &accepted, &effects);
    app_effect_list_free(&effects);
    assert(app->request == APP_REQUEST_PENDING);

    const NetEvent reply = {
        .type = NET_EVENT_REPLY,
        .response_status = status,
    };
    reduce_network(app, &reply, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
    assert(app->request == APP_REQUEST_IDLE);
}

/*
    A late frame-level error must cost the frame, not the match: the phase,
    the request state and the board all survive it.
*/
static void test_unsolicited_reply_keeps_the_game(void) {
    AppState app;
    app_init(&app);
    connect_app(&app);
    complete_control(&app, GAME_INTENT_CREATE, 201);
    complete_control(&app, GAME_INTENT_START, 200);
    assert(app.game_phase == APP_GAME_ACTIVE);

    AppEffectList effects;
    const NetEvent late = {
        .type = NET_EVENT_UNSOLICITED_REPLY,
        .response_status = 400,
    };
    reduce_network(&app, &late, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);

    assert(app.game_phase == APP_GAME_ACTIVE);
    assert(app.request == APP_REQUEST_IDLE);
    assert(app.connection == APP_CONNECTION_READY);
    app_free(&app);
}

static void test_reducer_game_lifecycle_and_input_queue(void) {
    AppState app;
    app_init(&app);
    connect_app(&app);

    complete_control(&app, GAME_INTENT_CREATE, 201);
    assert(app.game_phase == APP_GAME_LOBBY);
    complete_control(&app, GAME_INTENT_START, 200);
    assert(app.game_phase == APP_GAME_ACTIVE);

    AppEffectList effects;
    reduce_command(&app, GAME_INTENT_MOVE_LEFT, &effects);
    assert(effects.count == 1);
    assert(strcmp(effects.items[0].method, "MOVE") == 0);
    assert(effects.items[0].completion == CLIENT_REQUEST_COMPLETE_ON_SEND);
    app_effect_list_free(&effects);
    assert(app.request == APP_REQUEST_SUBMITTING);

    reduce_command(&app, GAME_INTENT_ROTATE_CW, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
    reduce_command(&app, GAME_INTENT_DROP_HARD, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
    assert(app.input_count == 2);

    const NetEvent accepted = {
        .type = NET_EVENT_SEND_ACCEPTED,
        .completion = CLIENT_REQUEST_COMPLETE_ON_SEND,
    };
    reduce_network(&app, &accepted, &effects);
    app_effect_list_free(&effects);
    assert(app.request == APP_REQUEST_SUBMITTING);

    const ProtoStateRequest state_value = make_state(true);
    const NetEvent state = {
        .type = NET_EVENT_STATE_PUSH,
        .state = state_value,
    };
    reduce_network(&app, &state, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
    assert(app.request == APP_REQUEST_SUBMITTING);
    assert(app.has_game_state);

    const NetEvent completed = {
        .type = NET_EVENT_SEND_COMPLETED,
        .completion = CLIENT_REQUEST_COMPLETE_ON_SEND,
    };
    reduce_network(&app, &completed, &effects);
    assert(effects.count == 1);
    assert(strcmp(effects.items[0].method, "ROTATE") == 0);
    assert(app.input_count == 1);
    app_effect_list_free(&effects);

    reduce_network(&app, &accepted, &effects);
    app_effect_list_free(&effects);
    reduce_network(&app, &completed, &effects);
    assert(effects.count == 1);
    assert(strcmp(effects.items[0].method, "DROP") == 0);
    assert(app.input_count == 0);
    app_effect_list_free(&effects);

    reduce_network(&app, &accepted, &effects);
    app_effect_list_free(&effects);
    reduce_network(&app, &completed, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
    assert(app.request == APP_REQUEST_IDLE);

    complete_control(&app, GAME_INTENT_LEAVE, 200);
    assert(app.game_phase == APP_GAME_NO_ROOM);
    assert(!app.has_game_state);
    app_free(&app);
}

static void test_failed_control_response_preserves_phase(void) {
    AppState app;
    app_init(&app);
    connect_app(&app);
    complete_control(&app, GAME_INTENT_CREATE, 409);
    assert(app.game_phase == APP_GAME_NO_ROOM);
    assert(app.last_response_status == 409);
    app_free(&app);
}

static void test_gameplay_input_queue_capacity(void) {
    AppState app;
    app_init(&app);
    connect_app(&app);
    app.game_phase = APP_GAME_ACTIVE;

    AppEffectList effects;
    reduce_command(&app, GAME_INTENT_MOVE_LEFT, &effects);
    assert(effects.count == 1);
    app_effect_list_free(&effects);

    for (size_t i = 0; i < APP_GAME_INPUT_QUEUE_CAPACITY; ++i) {
        reduce_command(&app, GAME_INTENT_MOVE_RIGHT, &effects);
        assert(effects.count == 0);
        app_effect_list_free(&effects);
    }
    assert(app.input_count == APP_GAME_INPUT_QUEUE_CAPACITY);

    reduce_command(&app, GAME_INTENT_DROP_HARD, &effects);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
    assert(app.input_count == APP_GAME_INPUT_QUEUE_CAPACITY);
    assert(strcmp(app.notification, "Gameplay input queue is full") == 0);
    app_free(&app);
}

int main(void) {
    test_command_parser_and_router();
    test_game_request_mapping();
    test_state_codec_and_inbound_policy();
    test_unsolicited_reply_keeps_the_game();
    test_reducer_game_lifecycle_and_input_queue();
    test_failed_control_response_preserves_phase();
    test_gameplay_input_queue_capacity();
    return 0;
}
