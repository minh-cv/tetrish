#include "config.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
