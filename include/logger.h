/*!
    @note this should be only under tetrisd but technically any program can use it. not sure worth it though, redirecting stdout/stderr is faster.

    however, this file should be shared so that other programs can use the same logging format.
*/

#ifndef TETRISH_LOGGER_H
#define TETRISH_LOGGER_H

#include <stdio.h>
typedef enum LoggerSeverity {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} LoggerSeverity;

/*!
    @brief set a global file descriptor for logging.
*/
int logger_init_ipc(const char* log_ipc);

/*!
    @brief set an output file for logging, could be stdout.
*/
void logger_init_file(FILE* file);

/*!
    @brief Produce a formatted heap-allocated string, with the current time.
*/
char* _logger_make_log(enum LoggerSeverity severity, const char* group, const char* file, int line, const char* fmt, ...);

/*!
    @brief sends log of a fully formatted string, consuming the string.
*/
int _logger_log(char* string);

int logger_free();

#define LOGGER_MAKE_LOG(severity, group, ...) \
    _logger_make_log((severity), (group), __FILE__, __LINE__, __VA_ARGS__)

#define LOGGER_LOG(severity, group, ...) \
    _logger_log((LOGGER_MAKE_LOG(severity, group, __VA_ARGS__)))

#endif