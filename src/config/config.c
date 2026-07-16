#include "config.h"
#include "dtor.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DTOR_WRAPPER_DEFINE(free)
static DTOR_WRAPPER_DEFINE(fclose)

int parse_line(char* line_start, char** argn, char** argv) {
    while (isspace((unsigned char)*line_start)) {
        line_start++;
    }

    if (*line_start == '\0') return -1;

    *argn = line_start;
    while (*line_start == '_' || isalnum((unsigned char)*line_start)) {
        line_start++;
    }
    if (*argn == line_start) {
        return -1;
    }

    if (*line_start != '=') {
        return -1;
    }
    *line_start = '\0';
    line_start++;
    *argv = line_start;
    while (*line_start != '\0' && *line_start != '\r') {
        line_start++;
    }
    *line_start = '\0';

    return 0;
}

int parse_content(Config* cfg, char* content) {
    cfg->argc = 0;
    char* save_ptr = NULL;
    for (; cfg->argc < CONFIG_MAX_ARGS;) {
        char* token = strtok_r(content, "\n", &save_ptr);
        content = NULL;
        if (token == NULL) {
            return 0;
        }
        char* argn;
        char* argv;
        if (parse_line(token, &argn, &argv) == -1) {
            continue;
        }
        size_t argnl = strlen(argn);
        size_t argvl = strlen(argv);
        char* argn_buf = malloc(argnl + 1);
        if (argn_buf == NULL) {
            config_free(cfg);
            return -1;
        }
        char* argv_buf = malloc(argvl + 1);
        if (argv_buf == NULL) {
            config_free(cfg);
            free(argn_buf);
            return -1;
        }
        memcpy(argn_buf, argn, argnl);
        argn_buf[argnl] = '\0';
        memcpy(argv_buf, argv, argvl);
        argv_buf[argvl] = '\0';

        cfg->argn[cfg->argc] = argn_buf;
        cfg->argv[cfg->argc] = argv_buf;
        cfg->argc++;
    }

    fputs("CONFIG_MAX_ARGS reached\n", stderr);
    return 0;
}

int config_make(Config* cfg, const char* tetrishrc_path) {
    DTOR_DEFINE(dtor, 10);

    FILE* tetrishrc_file = fopen(tetrishrc_path, "r");
    if (tetrishrc_file == NULL) {
        perror("fopen");
        DTOR_RETURN(dtor, -1);
    }
    DTOR_INSERT(dtor, fclose, tetrishrc_file);
    
    if (fseek(tetrishrc_file, 0, SEEK_END) == -1) {
        perror("fseek");
        DTOR_RETURN(dtor, -1);
    }
    long length = ftell(tetrishrc_file);
    if (length == -1) {
        perror("ftell");
        DTOR_RETURN(dtor, -1);
    }
    if (fseek(tetrishrc_file, 0, SEEK_SET) == -1) {
        perror("fseek");
        DTOR_RETURN(dtor, -1);
    }

    char* file_content = malloc((size_t)length + 1);
    if (file_content == NULL) {
        DTOR_RETURN(dtor, -1);
    }
    file_content[length] = '\0';
    DTOR_INSERT(dtor, free, file_content);
    if (fread(file_content, 1, (size_t)length, tetrishrc_file) != (size_t)length) {
        perror("fread");
        DTOR_RETURN(dtor, -1);
    }

    int rc = parse_content(cfg, file_content);
    DTOR_RETURN(dtor, rc);
}

size_t config_get_arg_idx(const Config* cfg, const char* directive) {
    for (size_t i = 0; i < cfg->argc; i++) {
        if (strcmp(cfg->argn[i], directive) == 0) {
            return i;
        }
    }

    return CONFIG_MAX_ARGS;
}

char* config_get_path(const Config* cfg, const char* directive, const char* project_dir) {
    size_t idx = config_get_arg_idx(cfg, directive);
    if (idx == CONFIG_MAX_ARGS) {
        return NULL;
    }
    const char* path = cfg->argv[idx];

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
    for (; cfg->argc > 0; cfg->argc--) {
        free(cfg->argn[cfg->argc - 1]);
        free(cfg->argv[cfg->argc - 1]);
    }
}

int config_get_long_arg(const Config* cfg, const char* directive, long* out) {
    size_t idx = config_get_arg_idx(cfg, directive);
    if (idx == CONFIG_MAX_ARGS) {
        return -1;
    }
    const char* arg = cfg->argv[idx];
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
