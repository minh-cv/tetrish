#include "logger.h"
#include "server.h"
#include "sig.h"
#include <assert.h>
#include <stdlib.h>

#ifndef TETRISH_TETRISD_NO_DAEMON
#include "daemon.h"
#endif

int main(void) {
    #ifndef TETRISH_TETRISD_NO_DAEMON
    switch (incantation()) {
        case 0:
        return 0;
        case -1:
        perror("incantation");
        return EXIT_FAILURE;
        case 1:
        break;
        default:
        assert(false);
    }
    #endif

    #ifdef TETRISH_TETRISD_NO_DAEMON
    // dev mode runs in the foreground; the logger otherwise drops everything
    logger_init_file(stderr);
    #endif

    // after incantation(), which sets SIGHUP to SIG_IGN
    if (set_sig_handler(SIGINT, sig_terminate) == -1 ||
        set_sig_handler(SIGTERM, sig_terminate) == -1 ||
        set_sig_handler(SIGHUP, sig_reload_config) == -1 ||
        set_sig_handler(SIGUSR1, sig_dump_state) == -1 ||
        set_sig_handler(SIGPIPE, SIG_IGN) == -1
    ) {
        LOGGER_PERROR("signal", "sigaction");
        return EXIT_FAILURE;
    }

    Server server;
    if (server_init(&server) == -1) {
        return EXIT_FAILURE;
    }

    while (server_tick(&server) == 0) {
    }

    server_free(&server);
    return 0;
}
