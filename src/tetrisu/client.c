#include "client.h"
#include "dtor.h"
#include <stdio.h>
#include <unistd.h>

static DTOR_WRAPPER_DEFINE(config_var_free)
static DTOR_WRAPPER_DEFINE(Connector_free)
static DTOR_WRAPPER_DEFINE(ServerIo_free)
static DTOR_WRAPPER_DEFINE(AuthData_free)
static DTOR_WRAPPER_DEFINE(HtttpData_free)
static DTOR_WRAPPER_DEFINE(InputData_free)
static DTOR_WRAPPER_DEFINE(InputEventQueue_free)
static DTOR_WRAPPER_DEFINE(AppData_free)

const char* client_fault_string(ClientFault fault) {
    switch (fault) {
    case FAULT_NONE: return "no fault";
    case FAULT_TRANSPORT: return "connection to the server was lost";
    case FAULT_PROTOCOL: return "the server sent something unusable";
    case FAULT_LOCAL: return "the client ran out of a local resource";
    }
    return "unknown fault";
}

int client_init(Client* client) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 1);

    client->fault = FAULT_NONE;

    if (config_var_init(&client->cfg) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, config_var_free, &client->cfg);

    if (Connector_init(&client->connector, client->cfg.address, client->cfg.port,
                       client->cfg.ca_path) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, Connector_free, &client->connector);

    if (ServerIo_init(&client->io, client->cfg.client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, ServerIo_free, &client->io);

    if (AuthData_init(&client->auth, client->connector.key, client->cfg.client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, AuthData_free, &client->auth);

    if (HtttpData_init(&client->htttp, client->cfg.client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, HtttpData_free, &client->htttp);

    if (InputData_init(&client->input, client->cfg.line_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, InputData_free, &client->input);

    if (InputEventQueue_init(&client->event_q, client->cfg.client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, InputEventQueue_free, &client->event_q);

    if (AppData_init(&client->app, client->cfg.client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, AppData_free, &client->app);

    if (UiData_init(&client->ui) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    Poller_init(&client->poller);
    Poller_accept(&client->poller, POLLER_SLOT_SERVER, client->connector.server_fd, POLLIN);
    Poller_accept(&client->poller, POLLER_SLOT_INPUT, STDIN_FILENO, POLLIN);

    AppData_note(&client->app, "connected to %s:%d", client->cfg.address, client->cfg.port);
    AppData_note(&client->app, "type `help` for commands");

    DTOR_RETURN(dtor, 0);
}

void client_free(Client* client) {
    // before anything prints, so a fault raised with the tui up still leaves a
    // usable terminal behind
    UiData_free(&client->ui);
    AppData_free(&client->app);
    InputData_reset(&client->input, &client->event_q);
    InputEventQueue_free(&client->event_q);
    InputData_free(&client->input);
    HtttpData_free(&client->htttp);
    AuthData_free(&client->auth);
    ServerIo_free(&client->io);
    Connector_free(&client->connector);
    config_var_free(&client->cfg);
}

/*
    Mode is decided during AppData_step and applied here, after rendering and
    before the resets: the input layer's stdin discipline and the UI layer's
    terminal state have to change together, and neither can change while the
    tick still holds bytes read under the old discipline.
*/
static void client_apply_mode(Client* client) {
    if (client->app.requested_mode == client->app.view.mode) {
        return;
    }

    const bool to_game = client->app.requested_mode == MODE_GAME;
    if (UiData_set_backend(&client->ui, to_game ? UI_BACKEND_TUI : UI_BACKEND_LINE,
                           client->cfg.frame_interval_ms) == -1 ||
        InputData_set_mode(&client->input, to_game ? INPUT_MODE_RAW : INPUT_MODE_LINE) == -1) {
        client->fault = FAULT_LOCAL;
        return;
    }

    if (to_game && client->ui.frame_timerfd != -1) {
        Poller_accept(&client->poller, POLLER_SLOT_FRAME_TIMER, client->ui.frame_timerfd, POLLIN);
    }
    else if (!to_game) {
        Poller_close(&client->poller, POLLER_SLOT_FRAME_TIMER);
    }
    client->app.view.mode = client->app.requested_mode;
}

bool client_tick(Client* client) {
    if (Poller_poll(&client->poller) == -1) {
        client->fault = FAULT_LOCAL;
        return false;
    }

    if (Poller_ready(&client->poller, POLLER_SLOT_INPUT, POLLIN | POLLHUP)) {
        InputData_read(&client->input, &client->event_q, &client->fault);
    }
    if (Poller_ready(&client->poller, POLLER_SLOT_SERVER, POLLIN | POLLHUP | POLLERR)) {
        ServerIo_read(&client->io, client->connector.server_fd, &client->io.read_q, &client->fault);
    }

    bool frame_tick = false;
    if (Poller_ready(&client->poller, POLLER_SLOT_FRAME_TIMER, POLLIN)) {
        UiData_read_frame_timer(&client->ui, &client->fault);
        frame_tick = true;
    }

    AuthData_decrypt(&client->auth, &client->io.read_q, &client->auth.decrypt_q, &client->fault);
    HtttpData_parse(&client->htttp, &client->auth.decrypt_q, &client->htttp.parsed_q, &client->fault);

    AppData_step(&client->app, &client->event_q, &client->htttp.parsed_q,
                 &client->app.request_q, frame_tick, &client->fault);

    HtttpData_serialize(&client->htttp, &client->app.request_q, &client->auth.encrypt_q,
                        &client->fault);
    AuthData_encrypt(&client->auth, &client->auth.encrypt_q, &client->io.write_q, &client->fault);
    ServerIo_write(&client->io, client->connector.server_fd, &client->io.write_q, &client->fault);

    UiData_render(&client->ui, &client->app.view, frame_tick, &client->fault);

    Poller_sync_interest(&client->poller, client->io.write_pending, client->input.eof);
    client_apply_mode(client);

    // parsed messages view into the decrypted frames, which view nothing, so
    // the order here is the reverse of the dependency
    HtttpData_reset(&client->htttp);
    AuthData_reset(&client->auth);
    ServerIo_reset(&client->io);
    InputData_reset(&client->input, &client->event_q);
    AppData_reset(&client->app);
    Poller_reset(&client->poller);

    return !client->app.quit && client->fault == FAULT_NONE;
}
