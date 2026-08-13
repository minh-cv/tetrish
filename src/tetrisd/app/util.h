#ifndef TETRISH_TETRISD_APP_UTIL_H
#define TETRISH_TETRISD_APP_UTIL_H

//! @brief a heap-allocated formatted string, or NULL on failure
__attribute__((format(printf, 1, 2)))
char* malloc_sprintf(const char* fmt, ...);

#endif
