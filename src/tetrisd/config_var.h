#ifndef TETRISH_TETRISD_CONFIG_VAR_H
#define TETRISH_TETRISD_CONFIG_VAR_H

#include <sys/epoll.h>

struct config_var {
    int port;
    char* address;
    char* cert_path;
    char* key_path;
    char* log_ipc;

    unsigned int max_fds;
    unsigned int max_events;
    unsigned int max_rooms;
    unsigned int logger_reconnect_seconds;
    unsigned int logger_capacity;
    unsigned int client_capacity;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif
