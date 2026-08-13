#ifndef TETRISH_TETRISD_CONFIG_VAR_H
#define TETRISH_TETRISD_CONFIG_VAR_H

/*!
    @brief reconfigurable behavior:
        ** @c cert_path , @c key_path : replace the credentials
        ** @c logger_reconnect_seconds
        ** @c log_ipc : on the next reconnection, use this as the path
        ** @c client_capacity : change the capacity of future connections
        ** @c room_tick_hz
        ** @c max_player_fd : reject future clients with fd larger than this
    
    The rest does not change on reconfiguration.
*/
struct config_var {
    int port;
    char* address;
    char* cert_path;
    char* key_path;
    char* log_ipc;
    char* control_ipc;

    unsigned int max_fds;
    unsigned int max_events;
    unsigned int max_rooms;
    unsigned int logger_reconnect_seconds;
    unsigned int logger_capacity;
    unsigned int client_capacity;
    unsigned int room_tick_hz;
    unsigned int max_player_fd;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif
