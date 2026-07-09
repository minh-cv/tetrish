#ifndef TETRISH_CONFIG_H
#define TETRISH_CONFIG_H

#include <stddef.h>

extern const char* const DEFAULT_REQUIRED_DIRECTIVES[];
extern const char* const DEFAULT_OPTIONAL_DIRECTIVES[];
extern const char* const DEFAULT_DEFAULT_ARGUMENTS[];

extern const size_t DEFAULT_REQUIRED_DIRECTIVES_LENGTH;
extern const size_t DEFAULT_OPTIONAL_DIRECTIVES_LENGTH;

const char** config_default();
const char** config(const char* const required_directives[], size_t required_directives_count,
           const char* const optional_directives[], const char* const default_arguments[],
           size_t optional_directives_count);
char* concat_path(const char* first, const char* second);

#endif