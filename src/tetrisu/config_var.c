#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(config_free)

/*!
    @see config_get_uint_arg (src/tetrisd/config_var.c) — intentional duplicate,
    since the per-daemon config layers deliberately do not share code
*/
static int config_get_uint_arg(const Config* config, const char* directive,
                               unsigned int fallback, unsigned int minimum, unsigned int* out) {
    long value;
    if (config_get_long_arg(config, directive, &value) == -1) {
        *out = fallback;
        return 0;
    }
    if (value < (long)minimum || value > INT_MAX) {
        fprintf(stderr, "%s invalid (minimum %u)\n", directive, minimum);
        return -1;
    }
    *out = (unsigned int)value;
    return 0;
}

int config_var_init(struct config_var* cfg_var) {
    DTOR_DEFINE(errdtor, 10);
    DTOR_DEFINE(dtor, 10);

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

    static const int LISTEN_PORT_DEFAULT = 4321;
    static const char* const ADDRESS_DEFAULT = "localhost";
    static const unsigned int CLIENT_CAPACITY_DEFAULT = 64;
    // a command and its response both sit in a queue in the same tick
    static const unsigned int CLIENT_CAPACITY_MIN = 4;
    static const unsigned int LINE_CAPACITY_DEFAULT = 4096;
    static const unsigned int LINE_CAPACITY_MIN = 64;
    static const unsigned int FRAME_INTERVAL_MS_DEFAULT = 16;
    static const unsigned int FRAME_INTERVAL_MS_MIN = 1;

    Config config;
    if (config_make(&config, tetrishrc_path) == -1) {
        fprintf(stderr, "cannot read %s\n", tetrishrc_path);
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

    const size_t address_idx = config_get_arg_idx(&config, "address");
    char* address;
    if (address_idx == CONFIG_MAX_ARGS) {
        const size_t address_length = strlen(ADDRESS_DEFAULT);
        address = malloc(address_length + 1);
        if (address == NULL) {
            DTOR_ERR_RETURN(errdtor, dtor, -1);
        }
        memcpy(address, ADDRESS_DEFAULT, address_length);
        address[address_length] = '\0';
    }
    else {
        address = config.argv[address_idx];
        config.argv[address_idx] = NULL;
    }
    DTOR_INSERT(errdtor, free, address);

    char* const ca_path = config_get_path(&config, "ca_path", project_dir);
    if (ca_path == NULL) {
        fprintf(stderr, "ca_path invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, ca_path);

    unsigned int client_capacity;
    if (config_get_uint_arg(&config, "client_capacity", CLIENT_CAPACITY_DEFAULT,
                            CLIENT_CAPACITY_MIN, &client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int line_capacity;
    if (config_get_uint_arg(&config, "line_capacity", LINE_CAPACITY_DEFAULT,
                            LINE_CAPACITY_MIN, &line_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int frame_interval_ms;
    if (config_get_uint_arg(&config, "frame_interval_ms", FRAME_INTERVAL_MS_DEFAULT,
                            FRAME_INTERVAL_MS_MIN, &frame_interval_ms) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    const struct config_var new_cfg = {
        .port = (int)listen_port_long,
        .address = address,
        .ca_path = ca_path,
        .client_capacity = client_capacity,
        .line_capacity = line_capacity,
        .frame_interval_ms = frame_interval_ms,
    };

    *cfg_var = new_cfg;

    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->ca_path);
    free(cfg->address);
}
