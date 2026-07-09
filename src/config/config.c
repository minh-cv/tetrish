#include "config.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* const DEFAULT_REQUIRED_DIRECTIVES[] = {
    "listen_port",
    "cert_path",
    "key_path",
    "ca_path",
    "log_path",
    "log_ipc",
};

const char* const DEFAULT_OPTIONAL_DIRECTIVES[] = {
    "max_clients",
    "max_events",
};

const char* const DEFAULT_DEFAULT_ARGUMENTS[] = {
    "1024",
    "64"
};

const size_t DEFAULT_REQUIRED_DIRECTIVES_LENGTH = sizeof(DEFAULT_REQUIRED_DIRECTIVES)/sizeof(DEFAULT_REQUIRED_DIRECTIVES[0]);
const size_t DEFAULT_OPTIONAL_DIRECTIVES_LENGTH = sizeof(DEFAULT_OPTIONAL_DIRECTIVES)/sizeof(DEFAULT_OPTIONAL_DIRECTIVES[0]);

const char** config(const char* const required_directives[], size_t required_directives_count,
           const char* const optional_directives[], const char* const default_arguments[],
           size_t optional_directives_count) {
    const char** config_list = malloc((required_directives_count + optional_directives_count)*sizeof(*config_list));

    if (config_list == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < required_directives_count; i++) {
        if ((config_list[i] = getenv(required_directives[i])) == NULL) {
            free(config_list);
            return NULL;
        }
    }

    for (size_t i = 0; i < optional_directives_count; i++) {
        const char* cfg = getenv(optional_directives[i]);
        if (cfg == NULL) {
            cfg = default_arguments[i];
        }
        config_list[i + required_directives_count] = cfg;
    }

    return config_list;
}

char* concat_path(const char* first, const char* second) {
    size_t first_len = strlen(first);
    size_t second_len = strlen(second);

    if (first_len > INT_MAX || second_len > INT_MAX) {
        return NULL;
    }

    if (first_len != 0 && first[first_len - 1] == '/') {
        first_len--;
    }

    char* path = malloc(first_len + second_len + 2);
    if (path == NULL) {
        return NULL;
    }

    sprintf(path, "%.*s/%.*s", (int)first_len, first, (int)second_len, second);

    return path;
}

const char** config_default() {
    return config(DEFAULT_REQUIRED_DIRECTIVES, DEFAULT_REQUIRED_DIRECTIVES_LENGTH, DEFAULT_OPTIONAL_DIRECTIVES, DEFAULT_DEFAULT_ARGUMENTS, DEFAULT_OPTIONAL_DIRECTIVES_LENGTH);
}