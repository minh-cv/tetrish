#ifndef TETRISH_TETRISD_SIG_H
#define TETRISH_TETRISD_SIG_H

#include <signal.h>

extern volatile sig_atomic_t running;
extern volatile sig_atomic_t dump_state;
extern volatile sig_atomic_t should_reload_config;

void sig_terminate(int signo);
void sig_reload_config(int sig);
void sig_dump_state(int sig);
int set_sig_handler(int sig, void (*handle_signal)(int));

#endif
