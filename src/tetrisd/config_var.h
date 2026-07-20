#ifndef TETRISH_TETRISD_CONFIG_VAR_H
#define TETRISH_TETRISD_CONFIG_VAR_H

#include "epollmanip.h"
#include "tetrissh.h"
#include <sys/epoll.h>

struct config_var {
    int port;
    char* address;
    char* cert_path;
    char* key_path;
    int max_events;
    int max_clients;
    char* log_ipc;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);
void reload_config(struct config_var* cfg, TetrishCredential* credential, struct epoll_event* events, Client* clients);

#endif