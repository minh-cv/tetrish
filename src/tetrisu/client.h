#ifndef TETRISH_TETRISU_CLIENT_H
#define TETRISH_TETRISU_CLIENT_H

#include "app_layer.h"
#include "auth.h"
#include "config_var.h"
#include "connector.h"
#include "htttp_layer.h"
#include "input_layer.h"
#include "poller.h"
#include "server_io.h"
#include "ui_layer.h"

/*!
    @brief The composition root, mirroring tetrisd's Server: it owns every
    layer and the queues that are not a layer's own, and it is the only place
    that knows the order the layers run in.
*/
typedef struct {
    struct config_var cfg;
    Connector connector;
    Poller poller;
    ServerIo io;
    AuthData auth;
    HtttpData htttp;
    InputData input;
    InputEventQueue event_q;
    AppData app;
    UiData ui;
    ClientFault fault;
} Client;

int client_init(Client* client);
void client_free(Client* client);

/*!
    @brief one tick, in the order documented in docs/tetrisu/layers.md

    @return false once the application asked to quit or a fault was raised
*/
bool client_tick(Client* client);

#endif
