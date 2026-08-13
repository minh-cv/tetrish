#ifndef TETRISH_TETRISD_CONFIG_VAR_H
#define TETRISH_TETRISD_CONFIG_VAR_H

/*!
    @brief reconfigurable behavior, named by their @c .tetrishrc directives:
        ** @c cert_path , @c key_path : replace the credentials
        ** @c tetrisd_logger_reconnect_seconds
        ** @c log_ipc : on the next reconnection, use this as the path
        ** @c tetrisd_client_capacity : change the capacity of future connections
        ** @c tetrisd_room_tick_hz
        ** @c tetrisd_max_player_fd : reject future clients with fd larger than this

    The rest does not change on reconfiguration.
*/
struct config_var {
    int port;
    char* address;
    char* cert_path;
    char* key_path;
    char* log_ipc;
    char* control_ipc;
    //! @brief POSIX mq name of the garbage queue; not a filesystem path
    char* garbage_ipc;

    unsigned int max_fds;
    unsigned int max_events;
    unsigned int max_rooms;
    unsigned int max_players_per_room;
    unsigned int logger_reconnect_seconds;
    unsigned int logger_capacity;
    unsigned int client_capacity;
    unsigned int room_tick_hz;
    unsigned int max_player_fd;
    //! @brief mq depth; like @c garbage_ipc , fixed at init, not reloadable
    unsigned int garbage_queue_depth;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif
