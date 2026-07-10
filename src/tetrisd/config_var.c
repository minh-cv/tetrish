#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(config_free)

int config_var_init(struct config_var* cfg_var) {
    DTOR_DEFINE(errdtor, 10);

    const char* const project_dir = getenv("PROJECT_DIR");
    if (project_dir == NULL) {
        fprintf(stderr, "PROJECT_DIR not set\n");
        DTOR_RETURN(errdtor, -1);
    }

    static const char* const required_directives[] = {
        "cert_path",
        "key_path",
    };

    static const char* const optional_directives[] = {
        "listen_port",
        "address",
        "max_events",
        "max_clients",
    };

    static const char* default_arguments[] = {
        "4321",
        "localhost",
        "64",
        "1024",
    };

    assert(sizeof(optional_directives)/sizeof(optional_directives[0]) == sizeof(default_arguments)/sizeof(default_arguments[0]));

    Config config = {
        sizeof(required_directives)/sizeof(required_directives[0]),
        sizeof(optional_directives)/sizeof(optional_directives[0]),
        required_directives,
        optional_directives,
        default_arguments,
    };

    if (config_make(&config) == -1) {
        fprintf(stderr, "cannot make config\n");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, config_free, &config);

    long listen_port_long;
    if (config_get_long_arg(&config, "listen_port", &listen_port_long) == -1) {
        fprintf(stderr, "listen_port invalid\n");
        DTOR_RETURN(errdtor, -1);
    }

    const char* address = config_get_arg(&config, "address");
    if (address == NULL) {
        fprintf(stderr, "address invalid\n");
        DTOR_RETURN(errdtor, -1);
    }

    char* cert_path = config_get_path(&config, "cert_path", project_dir);
    if (cert_path == NULL) {
        fprintf(stderr, "cert_path not set\n");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, free, cert_path);

    char* key_path = config_get_path(&config, "key_path", project_dir);
    if (key_path == NULL) {
        fprintf(stderr, "key_path not set\n");
        DTOR_RETURN(errdtor, -1);
    }
    DTOR_INSERT(errdtor, free, key_path);

    long max_events_long;
    if (config_get_long_arg(&config, "max_events", &max_events_long) == -1 || max_events_long > INT_MAX) {
        fprintf(stderr, "max_events invalid\n");
        DTOR_RETURN(errdtor, -1);
    }
    const int MAX_EVENTS = (int)max_events_long;

    long max_clients_long;
    if (config_get_long_arg(&config, "max_clients", &max_clients_long) == -1 || max_clients_long > INT_MAX) {
        fprintf(stderr, "max_clients invalid\n");
        DTOR_RETURN(errdtor, -1);
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

    return 0;
}

void config_var_free(struct config_var* cfg) {
    free(cfg->cert_path);
    free(cfg->key_path);
}