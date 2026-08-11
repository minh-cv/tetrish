#ifndef TETRISH_TETRISD_CONFIG_VAR_H
#define TETRISH_TETRISD_CONFIG_VAR_H

struct config_var {
    int port;
    char* address;
    char* cert_path;
    char* key_path;
    char* log_ipc;
    char* ctl_ipc;

    unsigned int max_fds;
    unsigned int max_events;
    unsigned int max_rooms;
    unsigned int logger_reconnect_seconds;
    unsigned int logger_capacity;
    unsigned int client_capacity;
    unsigned int app_arena_capacity;
    unsigned int tick_hz;
    unsigned int state_broadcast_divisor;
    unsigned int max_ctl_fds;
    unsigned int ctl_timeout_ms;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif
