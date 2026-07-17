#ifndef TETRISH_TETRISLOGD_CONFIG_VAR_H
#define TETRISH_TETRISLOGD_CONFIG_VAR_H

struct config_var {
    char* log_ipc;
    char* log_path;
};

int config_var_init(struct config_var *cfg_var);
void config_var_free(struct config_var *cfg);

#endif