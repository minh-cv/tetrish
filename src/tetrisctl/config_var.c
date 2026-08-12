#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include <stdio.h>
#include <stdlib.h>

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

    // same fallback as tetrisd's config_var so both ends agree on the socket
    char* control_ipc = config_get_path(&config, "control_ipc", project_dir);
    if (control_ipc == NULL) {
        control_ipc = concat_path(project_dir, "tetrisd.sock");
        if (control_ipc == NULL) {
            fputs("control_ipc invalid\n", stderr);
            DTOR_ERR_RETURN(errdtor, dtor, -1);
        }
    }

    cfg_var->control_ipc = control_ipc;

    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->control_ipc);
}
