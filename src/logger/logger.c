#include "common.h"
#include "logger.h"
#include "tetrissh.h"
#include <assert.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int log_fd = -1;
static FILE* log_file = NULL;
static bool is_file = false;

int logger_init_ipc(const char* log_ipc) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    size_t log_path_length = strlen(log_ipc);
    if (log_path_length >= sizeof(addr.sun_path)) {
        fputs("log_path too long", stderr);
        close(sockfd);
        return -1;
    }
    memcpy(addr.sun_path, log_ipc, log_path_length);
    addr.sun_path[log_path_length] = '\0';

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    log_fd = sockfd;
    return 0;
}

void logger_init_file(FILE *file) {
    log_file = file;
    is_file = true;
}

int logger_free() {
    if (is_file) {
        assert(log_file != NULL);
        fclose(log_file);
        log_file = NULL;
        return 0;        
    }
    assert(log_fd != -1);
    int close_val = close(log_fd);
    log_fd = -1;
    return close_val;
}

static const char* severity_to_string(enum LoggerSeverity severity) {
    switch (severity) {
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_INFO:
        return "INFO";
    case LOG_WARN:
        return "WARN";
    case LOG_ERROR:
        return "ERROR";
    default:
        assert(false);
        return "?";
    }
}

char* _logger_make_log(enum LoggerSeverity severity, const char* group, const char* file, int line, const char* fmt, ...) {
    char date[64];
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    // "xx:xx:xx xx/xx/xx"
    strftime(date, sizeof(date), "%T %D", &utc);

    const char* severity_str = severity_to_string(severity);
    const char* group_str = (group != NULL) ? group : "-";

    static const char* const PREFIX_FMT = "[%s][%s:%d][%s][%s] ";

    int prefix_len = snprintf(NULL, 0, PREFIX_FMT, date, file, line, severity_str, group_str);
    if (prefix_len < 0) {
        return NULL;
    }

    va_list args;
    va_start(args, fmt);

    va_list args_for_measuring;
    va_copy(args_for_measuring, args);
    int msg_len = vsnprintf(NULL, 0, fmt, args_for_measuring);
    va_end(args_for_measuring);

    if (msg_len < 0) {
        va_end(args);
        return NULL;
    }

    size_t total_len = (size_t)prefix_len + (size_t)msg_len;
    char* result = malloc(total_len + 1);
    if (result == NULL) {
        va_end(args);
        return NULL;
    }

    // snprintf's size includes room for the NUL terminator it writes; passing
    // prefix_len+1/msg_len+1 here (rather than the buffer's total size) keeps
    // each call confined to its own slice of `result`.
    snprintf(result, (size_t)prefix_len + 1, PREFIX_FMT, date, file, line, severity_str, group_str);
    vsnprintf(result + prefix_len, (size_t)msg_len + 1, fmt, args);

    va_end(args);
    return result;
}

static int send_uint32_t(int sockfd, uint32_t value) {
    uint8_t buf[4];
    encode_u32_be(buf, value);
    return send_all(sockfd, buf, sizeof(uint32_t));
}

static int send_frame(int sockfd, unsigned char* plaintext, uint32_t plaintext_length) {
    if (plaintext_length > FRAME_MAX || send_uint32_t(sockfd, (uint32_t)plaintext_length) == -1 ||
    send_all(sockfd, plaintext, plaintext_length) == -1) {
            return -1;
    }

    return 0;
}

int _logger_log(char* string) {
    if (is_file) {
        size_t len = strlen(string);
        int failed = (fwrite(string, 1, len, log_file) != len) || (fflush(log_file) == EOF);
        free(string);
        return failed ? -1 : 0;
    }

    if (string == NULL) {
        return -1;
    }

    size_t strlen_sz = strlen(string);
    if (strlen_sz > UINT32_MAX) {
        return -1;
    }

    int retval = send_frame(log_fd, (unsigned char*)string, (uint32_t)strlen_sz);

    free(string);
    
    return retval;
}