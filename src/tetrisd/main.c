#include "dtor.h"
#include "logger.h"
#include "server.h"
#include "sig.h"
#include "type.h"

#ifndef TETRISH_TETRISD_NO_DAEMON
#include "daemon.h"
#endif

static DTOR_WRAPPER_DEFINE(server_free)

static void free_ptr(void** ptr) {
    free(*ptr);
}

static DTOR_WRAPPER_DEFINE(free_ptr)

int main(void) {
    DTOR_DEFINE(dtor, 20);

    #ifndef TETRISH_TETRISD_NO_DAEMON
    switch (incantation()) {
        case 0:
        DTOR_RETURN(dtor, 0);
        case -1:
        perror("incantation");
        DTOR_RETURN(dtor, EXIT_FAILURE);
        case 1:
        break;
        default:
        assert(false);
    }
    #endif
    
    logger_set_log_handler(logger_log_null);

    if (set_sig_handler(SIGINT, sig_terminate) == -1 ||
        set_sig_handler(SIGTERM, sig_terminate) == -1 ||
        set_sig_handler(SIGPIPE, SIG_IGN) == -1 ||
        set_sig_handler(SIGHUP, sig_reload_config) == -1 ||
        set_sig_handler(SIGUSR1, sig_dump_state) == -1
    ) {
        LOGGER_PERROR("signal", "sigaction");
        DTOR_RETURN(dtor, EXIT_FAILURE);
    }
    
    Server server;
    if (server_init(&server) == -1) {
        DTOR_RETURN(dtor, EXIT_FAILURE);
    }
    DTOR_INSERT(dtor, server_free, &server);

    EpollEvents evs = {0};
    if (EpollEvents_calloc(&evs, server.cfg.max_events) == -1) {
        DTOR_RETURN(dtor, EXIT_FAILURE);
    }
    DTOR_INSERT(dtor, free_ptr, &evs.ptr);

    while (running) {
        server_poll(&server, &evs);

        if (should_reload_config) {
            should_reload_config = 0;
            server_reload_config(&server);
        }

        if (dump_state) {
            dump_state = 0;
            // TODO: dump rooms/players once libtetrisbrain is wired in.
            LOGGER_LOG(LOG_INFO, "main", "state dump requested");
        }
    }

    DTOR_RETURN(dtor, 0);
}
