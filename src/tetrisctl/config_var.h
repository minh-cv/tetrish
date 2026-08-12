#ifndef TETRISH_TETRISCTL_CONFIG_VAR_H
#define TETRISH_TETRISCTL_CONFIG_VAR_H

struct config_var {
    char* control_ipc;
};

int config_var_init(struct config_var* cfg_var);
void config_var_free(struct config_var* cfg);

#endif
