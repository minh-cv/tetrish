#include "app/util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

char* malloc_sprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int count = vsnprintf(NULL, 0, fmt, ap_copy);
    if (count < 0) {
        va_end(ap_copy);
        va_end(ap);
        return NULL;
    }
    va_end(ap_copy);
    char* buf = malloc((size_t)count + 1);
    if (buf == NULL) {
        va_end(ap);
        return NULL;
    }
    int result = vsprintf(buf, fmt, ap);
    va_end(ap);
    if (result < 0) {
        free(buf);
        return NULL;
    }
    return buf;
}
