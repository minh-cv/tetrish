#include "config_var.h"
#include "config.h"
#include "dtor.h"
#include "logger.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(config_free)

/*!
    @brief Read a count of at least `minimum`, falling back to `fallback` when
    the directive is absent or malformed.
*/
static int config_get_uint_arg(const Config* config, const char* directive,
                               unsigned int fallback, unsigned int minimum, unsigned int* out) {
    assert(fallback >= minimum);

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
    DTOR_DEFINE(errdtor, 25);
    DTOR_DEFINE(dtor, 25);

    const char* const project_dir = getenv("PROJECT_DIR");
    if (project_dir == NULL) {
        fprintf(stderr, "PROJECT_DIR invalid\n");
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    
    static const int LISTEN_PORT_DEFAULT = 4321;
    static const char* const ADDRESS_DEFAULT = "localhost";
    static const unsigned int MAX_EVENTS_DEFAULT = 64;
    static const unsigned int MAX_FDS_DEFAULT = 1024;
    static const unsigned int MAX_ROOMS_DEFAULT = 128;
    static const unsigned int LOGGER_RECONNECT_SECONDS_DEFAULT = 5;
    static const unsigned int LOGGER_CAPACITY_DEFAULT = 512;
    static const unsigned int CLIENT_CAPACITY_DEFAULT = 8;
    // sized for a full board snapshot per seat per tick, with slack
    static const unsigned int APP_ARENA_CAPACITY_DEFAULT = 1u << 20;
    static const unsigned int APP_ARENA_CAPACITY_MIN = 4096;
    // libtetrisbrain's gravity and lock counters are expressed in frames and
    // tuned around 60 Hz, so this is the rate they were written for
    static const unsigned int TICK_HZ_DEFAULT = 60;
    static const unsigned int STATE_BROADCAST_DIVISOR_DEFAULT = 2;
    // the nonce response queues two frames back to back, so anything smaller stalls the handshake.
    static const unsigned int CLIENT_CAPACITY_MIN = 2;

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
        DTOR_ERR_RETURN(errdtor, dtor, -1);
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

    unsigned int max_events;
    if (config_get_uint_arg(&config, "max_events", MAX_EVENTS_DEFAULT, 1, &max_events) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int max_fds;
    if (config_get_uint_arg(&config, "max_fds", MAX_FDS_DEFAULT, 1, &max_fds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int max_rooms;
    if (config_get_uint_arg(&config, "max_rooms", MAX_ROOMS_DEFAULT, 1, &max_rooms) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int logger_reconnect_seconds;
    if (config_get_uint_arg(&config, "logger_reconnect_seconds",
                            LOGGER_RECONNECT_SECONDS_DEFAULT, 1, &logger_reconnect_seconds) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int logger_capacity;
    if (config_get_uint_arg(&config, "logger_capacity", LOGGER_CAPACITY_DEFAULT, 1, &logger_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int client_capacity;
    if (config_get_uint_arg(&config, "client_capacity", CLIENT_CAPACITY_DEFAULT,
                            CLIENT_CAPACITY_MIN, &client_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int app_arena_capacity;
    if (config_get_uint_arg(&config, "app_arena_capacity", APP_ARENA_CAPACITY_DEFAULT,
                            APP_ARENA_CAPACITY_MIN, &app_arena_capacity) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int tick_hz;
    if (config_get_uint_arg(&config, "tick_hz", TICK_HZ_DEFAULT, 1, &tick_hz) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    unsigned int state_broadcast_divisor;
    if (config_get_uint_arg(&config, "state_broadcast_divisor", STATE_BROADCAST_DIVISOR_DEFAULT,
                            1, &state_broadcast_divisor) == -1) {
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }

    char* log_ipc = config_get_path(&config, "log_ipc", project_dir);
    if (log_ipc == NULL) {
        fputs("log_ipc invalid\n", stderr);
        DTOR_ERR_RETURN(errdtor, dtor, -1);
    }
    DTOR_INSERT(errdtor, free, log_ipc);

    struct config_var new_cfg = {
        .port = (int)listen_port_long,
        .address = address,
        .cert_path = cert_path,
        .key_path = key_path,
        .log_ipc = log_ipc,
        .max_fds = max_fds,
        .max_events = max_events,
        .max_rooms = max_rooms,
        .logger_reconnect_seconds = logger_reconnect_seconds,
        .logger_capacity = logger_capacity,
        .client_capacity = client_capacity,
        .app_arena_capacity = app_arena_capacity,
        .tick_hz = tick_hz,
        .state_broadcast_divisor = state_broadcast_divisor,
    };

    *cfg_var = new_cfg;

    DTOR_RETURN(dtor, 0);
}

void config_var_free(struct config_var* cfg) {
    free(cfg->cert_path);
    free(cfg->key_path);
    free(cfg->address);
    free(cfg->log_ipc);
}