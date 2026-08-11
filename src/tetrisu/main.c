#include "client.h"
#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t running = 1;

static void handle_stop(int signum) {
    (void)signum;
    running = 0;
}

/*
    SIGPIPE is ignored so a server that goes away shows up as EPIPE on the
    write, which the transport layer already turns into a fault.
*/
static int install_signal_handlers(void) {
    struct sigaction stop;
    sigemptyset(&stop.sa_mask);
    stop.sa_flags = 0;
    stop.sa_handler = handle_stop;

    if (sigaction(SIGINT, &stop, NULL) == -1 || sigaction(SIGTERM, &stop, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal SIGPIPE");
        return -1;
    }
    return 0;
}

int main(void) {
    if (install_signal_handlers() == -1) {
        return 1;
    }

    Client client;
    if (client_init(&client) == -1) {
        return 1;
    }

    while (running && client_tick(&client)) {
    }

    const ClientFault fault = client.fault;
    client_free(&client);

    if (fault != FAULT_NONE) {
        fprintf(stderr, "\n%s\n", client_fault_string(fault));
        return 1;
    }
    fputs("\n", stdout);
    return 0;
}
