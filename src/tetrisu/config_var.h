#ifndef TETRISH_TETRISU_CONFIG_VAR_H
#define TETRISH_TETRISU_CONFIG_VAR_H

struct config_var {
    int port;
    const char* address;
    char* ca_path;
};
int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif