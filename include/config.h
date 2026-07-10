#ifndef TETRISH_CONFIG_H
#define TETRISH_CONFIG_H

#include <stddef.h>

typedef struct Config {
    size_t required_directives_count;
    size_t optional_directives_count;
    const char* const* required_directives;
    const char* const* optional_directives;
    const char** arguments;
} Config;

/*! 
    @brief Make a full config from partial config.
    
    All fields in `Config` must be filled, with `arguments` being filled as the default arguments
    
    The returned config will have argument replaced with a malloc'd arguments list containing both actual required and optional arguments.
*/
int config_make(Config* cfg);

/*!
    @brief Find the argument of the corresponding directive, returning `NULL` if not found.

    Getting `NULL` should be considered a logic error if there is no need to detect whether a directive exists.
*/
const char* config_get_arg(const Config* cfg, const char* directive);

/*!
    @brief Read the corresponding path of directive, optionally prefix project_dir to the path if path is not an absolute path.

    Return a malloc'd null-terminated string.
*/
char* config_get_path(const Config* cfg, const char* directive, const char* project_dir);
int config_get_long_arg(const Config* cfg, const char* directive, long* out);
void config_free(Config* cfg);
char* concat_path(const char* first, const char* second);

#endif