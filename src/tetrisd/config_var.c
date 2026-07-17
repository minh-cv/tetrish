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
    
    static const int LISTEN_PORT_DEFAULT = 4321;
    static const char* const ADDRESS_DEFAULT = "localhost";
    static const int MAX_EVENTS_DEFAULT = 64;
    static const int MAX_CLIENTS_DEFAULT = 1024;

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

    long listen_port_long;
    if (config_get_long_arg(&config, "listen_port", &listen_port_long) == -1) {
        listen_port_long = LISTEN_PORT_DEFAULT;
    }
    if (listen_port_long > UINT16_MAX || listen_port_long <= 0) {
        fprintf(stderr, "listen_port invalid\n");
        DTOR_RETURN(errdtor, -1);
    }

    size_t address_idx = config_get_arg_idx(&config, "address");
    char* address;
    if (address_idx != CONFIG_MAX_ARGS) {
        address = config.argv[address_idx];
        config.argv[address_idx] = NULL;
    }
    else {
        size_t address_default_len = strlen(ADDRESS_DEFAULT);
        address = malloc(address_default_len + 1);
        if (address == NULL) {
            DTOR_ERR_RETURN(errdtor, dtor, -1);
        }
        memcpy(address, ADDRESS_DEFAULT, address_default_len);
        address[address_default_len] = '\0';
    }
    DTOR_INSERT(errdtor, free, address);

    char* cert_path = config_get_path(&config, "cert_path", project_dir);
    if (cert_path == NULL) {
        fprintf(stderr, "cert_path invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, cert_path);

    char* key_path = config_get_path(&config, "key_path", project_dir);
    if (key_path == NULL) {
        fprintf(stderr, "key_path invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, key_path);

    long max_events_long;
    if (config_get_long_arg(&config, "max_events", &max_events_long) == -1) {
        max_events_long = MAX_EVENTS_DEFAULT;
    }
    if (max_events_long > INT_MAX) {
        fprintf(stderr, "max_events invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    const int MAX_EVENTS = (int)max_events_long;

    long max_clients_long;
    if (config_get_long_arg(&config, "max_clients", &max_clients_long) == -1) {
        max_clients_long = MAX_CLIENTS_DEFAULT;
    }
    if (max_clients_long > INT_MAX) {
        fprintf(stderr, "max_clients invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    const int MAX_CLIENTS = (int)max_clients_long;

    struct config_var new_cfg = {
        (int)listen_port_long,
        address,
        cert_path,
        key_path,
        MAX_EVENTS,
        MAX_CLIENTS,
    };

    *cfg_var = new_cfg;

    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->cert_path);
    free(cfg->key_path);
    free(cfg->address);
}