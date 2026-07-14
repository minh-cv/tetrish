#ifndef TETRISH_CONFIG_H
#define TETRISH_CONFIG_H

#include <stddef.h>

#define CONFIG_MAX_ARGS 32u

typedef struct Config {
    char* argn[CONFIG_MAX_ARGS];
    char* argv[CONFIG_MAX_ARGS];
    size_t argc;
} Config;

/*! 
    @brief Read the config from the file path.    
    The returned config will have argument replaced with a malloc'd arguments list containing both actual required and optional arguments.
*/
int config_make(Config* cfg, const char* tetrishrc_path);

/*!
    @brief Find the argument of the corresponding directive, returning CONFIG_MAX_ARGS if not found.
*/
size_t config_get_arg_idx(const Config* cfg, const char* directive);

/*!
    @brief Read the corresponding path of directive, optionally prefix project_dir to the path if path is not an absolute path.

    Return a malloc'd null-terminated string.
*/
char* config_get_path(const Config* cfg, const char* directive, const char* project_dir);
int config_get_long_arg(const Config* cfg, const char* directive, long* out);
void config_free(Config* cfg);
char* concat_path(const char* first, const char* second);

#endif