#ifndef TETRISH_TETRISU_CONFIG_VAR_H
#define TETRISH_TETRISU_CONFIG_VAR_H

struct config_var {
    int port;
    char* address;
    char* ca_path;

    unsigned int client_capacity;     // depth of every per-tick queue
    unsigned int line_capacity;       // longest line the shell accepts
    unsigned int frame_interval_ms;   // game-mode render and input cadence
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif
