#include "app.h"
#include "command/parser.h"
#include "command/router.h"
#include "net/htttp_codec.h"
#include "net/inbound_policy.h"

#include "htttp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_command_parser_and_router(void) {
    static const char input[] = "htttp \"hello world\" 'from tetrisu'";
    CommandArgv argv = {0};
    assert(command_argv_parse(input, strlen(input), &argv) == COMMAND_PARSE_OK);
    assert(argv.argc == 3);
    assert(strcmp(argv.argv[1], "hello world") == 0);

    ParsedCommand command = {0};
    assert(command_route(&argv, &command) == COMMAND_ROUTE_OK);
    assert(command.type == COMMAND_SEND_RAW);
    assert(strcmp(command.argument, "hello world from tetrisu") == 0);
    parsed_command_free(&command);
    command_argv_free(&argv);

    static const char escaped[] = "htttp \"line\\n\\\"quoted\\\"\"";
    assert(command_argv_parse(escaped, strlen(escaped), &argv) == COMMAND_PARSE_OK);
    assert(strcmp(argv.argv[1], "line\n\"quoted\"") == 0);
    command_argv_free(&argv);
    static const char unterminated[] = "htttp 'open";
    assert(command_argv_parse(
        unterminated,
        sizeof(unterminated) - 1,
        &argv
    ) == COMMAND_PARSE_UNTERMINATED_QUOTE);

    static const char set_name[] = "set-name Reference Player";
    assert(command_argv_parse(set_name, sizeof(set_name) - 1, &argv) == COMMAND_PARSE_OK);
    assert(command_route(&argv, &command) == COMMAND_ROUTE_OK);
    assert(command.type == COMMAND_SET_NAME);
    assert(strcmp(command.argument, "Reference Player") == 0);
    parsed_command_free(&command);
    command_argv_free(&argv);
}

static void test_codec_round_trip(void) {
    static const unsigned char body[] = "payload";
    const ClientRequest request = {
        .method = "HTTTP",
        .path = "",
        .body = body,
        .body_len = sizeof(body) - 1,
        .content_type = "text/plain",
    };
    OwnedBytes bytes = {0};
    assert(htttp_codec_encode_request(&request, &bytes) == 0);
    OwnedHtttpMessage decoded = {0};
    assert(htttp_codec_decode_owned(&bytes, &decoded) == 0);
    assert(decoded.view.is_request);
    assert(strcmp(decoded.view.request.method, "HTTTP") == 0);
    const OwnedBytes decoded_body = htttp_codec_borrow_body(&decoded);
    assert(decoded_body.len == sizeof(body) - 1);
    assert(memcmp(decoded_body.ptr, body, sizeof(body) - 1) == 0);
    owned_htttp_message_free(&decoded);
}

static OwnedHtttpMessage make_request(const char* method, const char* body) {
    char body_length[32];
    (void)snprintf(body_length, sizeof(body_length), "%zu", strlen(body));
    const HtttpMessage source = {
        .request = {
            .method = method,
            .path = "",
            .header = {{"Content-Length", body_length}},
            .header_count = 1,
            .body = (const unsigned char*)body,
            .body_len = strlen(body),
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

static void test_inbound_policy(void) {
    OwnedHtttpMessage state = make_request("STATE", "{\"piece\":\"T\"}");
    assert(net_inbound_classify(&state, false, false) == NET_INBOUND_STATE_PUSH);
    assert(net_inbound_classify(&state, true, false) == NET_INBOUND_STATE_PUSH);
    owned_htttp_message_free(&state);

    OwnedHtttpMessage unknown = make_request("PING", "x");
    assert(net_inbound_classify(&unknown, false, false) == NET_INBOUND_REJECT);
    assert(net_inbound_classify(&unknown, true, false) == NET_INBOUND_REJECT);
    assert(net_inbound_classify(&unknown, true, true) == NET_INBOUND_LEGACY_ECHO);
    owned_htttp_message_free(&unknown);
}

static void reduce_network(AppState* app, const NetEvent* network) {
    AppEffectList effects;
    app_effect_list_init(&effects);
    const AppEvent event = {.type = APP_EVENT_NETWORK, .data.network = network};
    assert(app_reduce(app, &event, &effects) == 0);
    assert(effects.count == 0);
    app_effect_list_free(&effects);
}

static void test_state_push_preserves_pending_request(void) {
    AppState app;
    app_init(&app);

    NetEvent connected = {.type = NET_EVENT_CONNECTED};
    reduce_network(&app, &connected);
    NetEvent accepted = {.type = NET_EVENT_SEND_ACCEPTED};
    reduce_network(&app, &accepted);
    assert(app.connection == APP_CONNECTION_READY);
    assert(app.request == APP_REQUEST_PENDING);

    static const unsigned char body[] = "{\"board\":1}";
    NetEvent state = {.type = NET_EVENT_STATE_PUSH};
    state.payload.ptr = (unsigned char*)body;
    state.payload.len = sizeof(body) - 1;
    reduce_network(&app, &state);
    assert(app.request == APP_REQUEST_PENDING);
    assert(app.remote_state.len == sizeof(body) - 1);
    assert(memcmp(app.remote_state.ptr, body, sizeof(body) - 1) == 0);

    app_free(&app);
}

static void test_set_name_reduces_to_typed_request(void) {
    AppState app;
    app_init(&app);
    app.connection = APP_CONNECTION_READY;
    static char name[] = "Reference";
    const ParsedCommand command = {
        .type = COMMAND_SET_NAME,
        .argument = name,
        .argument_len = sizeof(name) - 1,
    };
    const AppEvent event = {
        .type = APP_EVENT_COMMAND_SUBMITTED,
        .data.command = &command,
    };
    AppEffectList effects;
    app_effect_list_init(&effects);
    assert(app_reduce(&app, &event, &effects) == 0);
    assert(effects.count == 1);
    assert(effects.items[0].type == APP_EFFECT_NET_SEND);
    assert(strcmp(effects.items[0].method, "SET_PLAYER_NAME") == 0);
    assert(effects.items[0].payload.len == sizeof(name) - 1);
    assert(app.request == APP_REQUEST_SUBMITTING);
    app_effect_list_free(&effects);
    app_free(&app);
}

int main(void) {
    test_command_parser_and_router();
    test_codec_round_trip();
    test_inbound_policy();
    test_state_push_preserves_pending_request();
    test_set_name_reduces_to_typed_request();
    return 0;
}
