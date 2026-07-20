#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(config_free)

int config_var_init(struct config_var* cfg_var) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 10);

    const char* const project_dir = getenv("PROJECT_DIR");
    if (project_dir == NULL) {
        fprintf(stderr, "PROJECT_DIR invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    char* const tetrishrc_path = concat_path(project_dir, ".tetrishrc");
    if (tetrishrc_path == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(dtor, free, tetrishrc_path);

    Config config;
    if (config_make(&config, tetrishrc_path) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(dtor, config_free, &config);

    static const int LOGD_MAX_CLIENTS_DEFAULT = 64;

    char* log_ipc = config_get_path(&config, "log_ipc", project_dir);
    if (log_ipc == NULL) {
        fputs("log_ipc invalid\n", stderr);
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, log_ipc);

    char* log_path = config_get_path(&config, "log_path", project_dir);
    if (log_path == NULL) {
        fputs("log_path invalid\n", stderr);
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, log_path);

    long logd_max_clients_long;
    if (config_get_long_arg(&config, "logd_max_clients", &logd_max_clients_long) == -1) {
        logd_max_clients_long = LOGD_MAX_CLIENTS_DEFAULT;
    }
    if (logd_max_clients_long <= 0 || logd_max_clients_long > INT_MAX) {
        fprintf(stderr, "logd_max_clients invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    const int LOGD_MAX_CLIENTS = (int)logd_max_clients_long;

    struct config_var new_cfg = {
        log_ipc,
        log_path,
        LOGD_MAX_CLIENTS,
    };

    *cfg_var = new_cfg;

    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->log_ipc);
    free(cfg->log_path);
}