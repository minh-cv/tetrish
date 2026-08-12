#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include <assert.h>
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
        fprintf(stderr, "PROJECT_DIR not set\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    char* tetrishrc_path = concat_path(project_dir, ".tetrishrc");
    if (tetrishrc_path == NULL) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(dtor, free, tetrishrc_path);

    static const int LISTEN_PORT_DEFAULT = 4321;
    static const char* ADDRESS_DEFAULT = "localhost";

    Config config;

    if (config_make(&config, tetrishrc_path) == -1) {
        fprintf(stderr, "cannot make config\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(dtor, config_free, &config);

    long listen_port_long;
    if (config_get_long_arg(&config, "listen_port", &listen_port_long) == -1) {
        listen_port_long = LISTEN_PORT_DEFAULT;
    }
    if (listen_port_long > UINT16_MAX || listen_port_long <= 0) {
        fprintf(stderr, "listen_port invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    size_t address_idx = config_get_arg_idx(&config, "address");
    char* address;
    if (address_idx == CONFIG_MAX_ARGS) {
        size_t address_length = strlen(ADDRESS_DEFAULT);
        address = malloc(address_length + 1);
        if (address == NULL) {
            DTOR_ERR_RETURN(errdtor, dtor, -1);
        }
        DTOR_INSERT(errdtor, free, address);
        memcpy(address, ADDRESS_DEFAULT, address_length);
        address[address_length] = '\0';
    }
    else {
        address = config.argv[address_idx];
        config.argv[address_idx] = NULL;
    }

    char* cert_path = config_get_path(&config, "ca_path", project_dir);
    if (cert_path == NULL) {
        fprintf(stderr, "ca_path invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, cert_path);

    struct config_var new_cfg = {
        (int)listen_port_long,
        address,
        cert_path,
    };

    *cfg_var = new_cfg;

    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->ca_path);
    free(cfg->address);
}