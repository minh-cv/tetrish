#include "sig.h"
#include <stdlib.h>

volatile sig_atomic_t running = 1;
void sig_terminate(int signo) {
    (void)signo;
    running = 0;
}

volatile sig_atomic_t should_reload_config = 0;
void sig_reload_config(int sig) {
    (void)sig;
    should_reload_config = 1;
}

volatile sig_atomic_t dump_state = 0;
void sig_dump_state(int sig) {
    (void)sig;
    dump_state = 1;
}

int set_sig_handler(int sig, void (*handle_signal)(int)) {
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(sig, &sa, NULL) == -1) {
        return -1;
    }
    return 0;
}
