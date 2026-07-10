#ifndef TETRISH_TETRISD_CONFIG_VAR_H
#define TETRISH_TETRISD_CONFIG_VAR_H

struct config_var {
    int port;
    const char* address;
    char* cert_path;
    char* key_path;
    int max_events;
    int max_clients;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif