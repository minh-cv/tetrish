#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include <stdio.h>
#include <stdlib.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(config_free)

int config_var_init(struct config_var* cfg_var) {
    DTOR_DEFINE(errdtor, 5);
    DTOR_DEFINE(dtor, 5);

    const char* const project_dir = getenv("PROJECT_DIR");
    if (project_dir == NULL) {
        fprintf(stderr, "PROJECT_DIR not set\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    char* const tetrishrc_path = concat_path(project_dir, ".tetrishrc");
    if (tetrishrc_path == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(dtor, free, tetrishrc_path);

    Config config;
    if (config_make(&config, tetrishrc_path) == -1) {
        fprintf(stderr, "cannot read %s\n", tetrishrc_path);
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(dtor, config_free, &config);

    // the only directive the control CLI needs; --socket overrides it
    char* const ctl_ipc = config_get_path(&config, "ctl_ipc", project_dir);
    if (ctl_ipc == NULL) {
        fprintf(stderr, "ctl_ipc invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, ctl_ipc);

    cfg_var->ctl_ipc = ctl_ipc;
    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->ctl_ipc);
    cfg->ctl_ipc = NULL;
}
