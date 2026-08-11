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

    /*
        Until the configuration is parsed the log socket's path is unknown, so
        everything logged before LoggerData_init goes here instead of being
        dropped by the null handler. Not paired with logger_free_file, which
        would fclose(stderr); LoggerData_init replaces the handler in place.
    */
    logger_init_file(stderr);

    if (set_sig_handler(SIGINT, sig_terminate) == -1 ||
        set_sig_handler(SIGTERM, sig_terminate) == -1 ||
        set_sig_handler(SIGPIPE, SIG_IGN) == -1
    ) {
        LOGGER_PERROR("signal", "sigaction");
        return EXIT_FAILURE;
    }

    Server server;
    if (server_init(&server) == -1) {
        return EXIT_FAILURE;
    }

    while (running && !server_should_stop(&server)) {
        server_tick(&server);
    }

    server_free(&server);
    return 0;
}
