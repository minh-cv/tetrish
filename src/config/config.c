#include "config.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int config_make(Config* cfg) {
    const char** config_list = malloc((cfg->required_directives_count + cfg->optional_directives_count)*sizeof(*config_list));

    if (config_list == NULL) {
        return -1;
    }

    for (size_t i = 0; i < cfg->required_directives_count; i++) {
        if ((config_list[i] = getenv(cfg->required_directives[i])) == NULL) {
            free(config_list);
            return -1;
        }
    }

    for (size_t i = 0; i < cfg->optional_directives_count; i++) {
        const char* c = getenv(cfg->optional_directives[i]);
        if (c == NULL) {
            c = cfg->arguments[i];
        }
        config_list[i + cfg->required_directives_count] = c;
    }

    cfg->arguments = config_list;
    return 0;
}

const char* config_get_arg(const Config* cfg, const char* directive) {
    for (size_t i = 0; i < cfg->required_directives_count; i++) {
        if (strcmp(cfg->required_directives[i], directive) == 0) {
            return cfg->arguments[i];
        }
    }

    for (size_t i = 0; i < cfg->optional_directives_count; i++) {
        if (strcmp(cfg->optional_directives[i], directive) == 0) {
            return cfg->arguments[i + cfg->required_directives_count];
        }
    }

    return NULL;
}

char* config_get_path(const Config* cfg, const char* directive, const char* project_dir) {
    const char* path = config_get_arg(cfg, directive);
    if (path == NULL) {
        return NULL;
    }

    if (path[0] == '/') {
        char* new_path = malloc(strlen(path) + 1);
        if (new_path == NULL) {
            return NULL;
        }

        strcpy(new_path, path);
        return new_path;
    }

    return concat_path(project_dir, path);
}

void config_free(Config *cfg) {
    free(cfg->arguments);
}

int config_get_long_arg(const Config* cfg, const char* directive, long* out) {
    const char* arg = config_get_arg(cfg, directive);
    if (arg == NULL) {
        return -1;
    }
    char* endptr;
    errno = 0;
    *out = strtol(arg, &endptr, 0);

    if (errno == ERANGE) {
        return -1;
    }
    if (*arg == '\0' || *endptr != '\0') {
        return -1;
    }

    return 0;
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
