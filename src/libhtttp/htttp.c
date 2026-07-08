#include "htttp.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static int copy_checked(char *destination, size_t size, const char *source, size_t length) {
    if (length >= size) {
        return -1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

static const unsigned char *find_bytes(const unsigned char *buffer, size_t length,
                                       const char *needle, size_t needle_length) {
    size_t offset;
    if (needle_length > length) {
        return NULL;
    }
    for (offset = 0; offset <= length - needle_length; offset++) {
        if (memcmp(buffer + offset, needle, needle_length) == 0) {
            return buffer + offset;
        }
    }
    return NULL;
}

void htttp_message_init(htttp_message_t *message) {
    memset(message, 0, sizeof(*message));
}

void htttp_message_free(htttp_message_t *message) {
    free(message->body);
    htttp_message_init(message);
}

int htttp_add_header(htttp_message_t *message, const char *name, const char *value) {
    htttp_header_t *header;
    if (message->header_count == HTTTP_MAX_HEADERS ||
        strlen(name) >= HTTTP_MAX_HEADER_NAME || strlen(value) >= HTTTP_MAX_HEADER_VALUE ||
        strchr(name, ':') != NULL || *name == '\0') {
        return -1;
    }
    header = &message->headers[message->header_count++];
    snprintf(header->name, sizeof(header->name), "%s", name);
    snprintf(header->value, sizeof(header->value), "%s", value);
    return 0;
}

const char *htttp_get_header(const htttp_message_t *message, const char *name) {
    size_t index;
    for (index = 0; index < message->header_count; index++) {
        if (strcasecmp(message->headers[index].name, name) == 0) {
            return message->headers[index].value;
        }
    }
    return NULL;
}

int htttp_set_body(htttp_message_t *message, const void *body, size_t length) {
    unsigned char *copy = NULL;
    free(message->body);
    message->body = NULL;
    message->body_length = 0;
    if (length == 0) {
        return 0;
    }
    copy = malloc(length + 1U);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, body, length);
    copy[length] = '\0';
    message->body = copy;
    message->body_length = length;
    return 0;
}

static int parse_start_line(const unsigned char *line, size_t length, htttp_message_t *message) {
    const unsigned char *first_space = memchr(line, ' ', length);
    const unsigned char *second_space;
    if (first_space == NULL) {
        return -1;
    }
    second_space = memchr(first_space + 1, ' ', length - (size_t)(first_space + 1 - line));
    if (second_space == NULL) {
        return -1;
    }
    if ((size_t)(first_space - line) == strlen("HTTTP/1.0") &&
        memcmp(line, "HTTTP/1.0", strlen("HTTTP/1.0")) == 0) {
        char status[4];
        char *end;
        long number;
        message->kind = HTTTP_RESPONSE;
        if ((size_t)(second_space - first_space - 1) != 3 ||
            copy_checked(status, sizeof(status), (const char *)first_space + 1, 3) != 0 ||
            copy_checked(message->reason, sizeof(message->reason), (const char *)second_space + 1,
                         length - (size_t)(second_space + 1 - line)) != 0) {
            return -1;
        }
        number = strtol(status, &end, 10);
        if (*end != '\0' || number < 100 || number > 999) {
            return -1;
        }
        message->status = (int)number;
        return 0;
    }
    message->kind = HTTTP_REQUEST;
    if (length - (size_t)(second_space + 1 - line) != strlen("HTTTP/1.0") ||
        memcmp(second_space + 1, "HTTTP/1.0", strlen("HTTTP/1.0")) != 0 ||
        copy_checked(message->method, sizeof(message->method), (const char *)line,
                     (size_t)(first_space - line)) != 0 ||
        copy_checked(message->path, sizeof(message->path), (const char *)first_space + 1,
                     (size_t)(second_space - first_space - 1)) != 0) {
        return -1;
    }
    return 0;
}

static int parse_content_length(const htttp_message_t *message, size_t *result) {
    const char *value = htttp_get_header(message, "Content-Length");
    char *end;
    unsigned long parsed;
    if (value == NULL) {
        *result = 0;
        return 0;
    }
    parsed = strtoul(value, &end, 10);
    if (*value == '\0' || *end != '\0' || parsed > HTTTP_MAX_MESSAGE) {
        return -1;
    }
    *result = (size_t)parsed;
    return 0;
}

int htttp_parse(const unsigned char *buffer, size_t length, htttp_message_t *message) {
    const unsigned char *headers_end;
    const unsigned char *line_end;
    const unsigned char *cursor;
    size_t body_length;
    htttp_message_init(message);
    if (length == 0 || length > HTTTP_MAX_MESSAGE) {
        return -1;
    }
    headers_end = find_bytes(buffer, length, "\r\n\r\n", 4);
    if (headers_end == NULL) {
        return -1;
    }
    line_end = find_bytes(buffer, (size_t)(headers_end - buffer) + 2U, "\r\n", 2);
    if (line_end == NULL || parse_start_line(buffer, (size_t)(line_end - buffer), message) != 0) {
        goto fail;
    }
    cursor = line_end + 2;
    while (cursor < headers_end) {
        const unsigned char *colon;
        size_t line_length;
        line_end = find_bytes(cursor, (size_t)(headers_end - cursor) + 2U, "\r\n", 2);
        if (line_end == NULL) {
            goto fail;
        }
        line_length = (size_t)(line_end - cursor);
        colon = memchr(cursor, ':', line_length);
        if (colon == NULL || colon == cursor) {
            goto fail;
        }
        while (colon + 1 < line_end && (colon[1] == ' ' || colon[1] == '\t')) {
            colon++;
        }
        {
            char name[HTTTP_MAX_HEADER_NAME];
            char value[HTTTP_MAX_HEADER_VALUE];
            const unsigned char *separator = memchr(cursor, ':', line_length);
            const unsigned char *value_start = separator + 1;
            while (value_start < line_end && (*value_start == ' ' || *value_start == '\t')) {
                value_start++;
            }
            if (copy_checked(name, sizeof(name), (const char *)cursor,
                             (size_t)(separator - cursor)) != 0 ||
                copy_checked(value, sizeof(value), (const char *)value_start,
                             (size_t)(line_end - value_start)) != 0 ||
                (strcasecmp(name, "Content-Length") == 0 &&
                 htttp_get_header(message, "Content-Length") != NULL) ||
                htttp_add_header(message, name, value) != 0) {
                goto fail;
            }
        }
        cursor = line_end + 2;
    }
    if (parse_content_length(message, &body_length) != 0 ||
        length - (size_t)(headers_end + 4 - buffer) != body_length ||
        htttp_set_body(message, headers_end + 4, body_length) != 0) {
        goto fail;
    }
    return 0;
fail:
    htttp_message_free(message);
    return -1;
}

static int append(unsigned char *buffer, size_t capacity, size_t *used,
                  const char *format, ...) {
    int written;
    va_list arguments;
    if (*used >= capacity) {
        return -1;
    }
    va_start(arguments, format);
    written = vsnprintf((char *)buffer + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= capacity - *used) {
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

int htttp_serialize(const htttp_message_t *message, unsigned char **buffer, size_t *length) {
    unsigned char *output = malloc(HTTTP_MAX_MESSAGE + 1U);
    size_t used = 0;
    size_t index;
    int has_content_length = htttp_get_header(message, "Content-Length") != NULL;
    if (output == NULL) {
        return -1;
    }
    if ((message->kind == HTTTP_REQUEST &&
         append(output, HTTTP_MAX_MESSAGE + 1U, &used, "%s %s HTTTP/1.0\r\n",
                message->method, message->path) != 0) ||
        (message->kind == HTTTP_RESPONSE &&
         append(output, HTTTP_MAX_MESSAGE + 1U, &used, "HTTTP/1.0 %d %s\r\n",
                message->status, message->reason) != 0)) {
        goto fail;
    }
    for (index = 0; index < message->header_count; index++) {
        if (append(output, HTTTP_MAX_MESSAGE + 1U, &used, "%s: %s\r\n",
                   message->headers[index].name, message->headers[index].value) != 0) {
            goto fail;
        }
    }
    if (message->body_length > 0 && !has_content_length &&
        append(output, HTTTP_MAX_MESSAGE + 1U, &used, "Content-Length: %zu\r\n",
               message->body_length) != 0) {
        goto fail;
    }
    if (append(output, HTTTP_MAX_MESSAGE + 1U, &used, "\r\n") != 0 ||
        used + message->body_length > HTTTP_MAX_MESSAGE) {
        goto fail;
    }
    if (message->body_length > 0) {
        memcpy(output + used, message->body, message->body_length);
    }
    used += message->body_length;
    *buffer = output;
    *length = used;
    return 0;
fail:
    free(output);
    return -1;
}

static void add_date(htttp_message_t *message) {
    char date[64];
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    htttp_add_header(message, "Date", date);
}

int htttp_make_response(htttp_message_t *message, int status, const char *reason,
                        const char *body, const char *content_type) {
    htttp_message_init(message);
    message->kind = HTTTP_RESPONSE;
    message->status = status;
    snprintf(message->reason, sizeof(message->reason), "%s", reason);
    add_date(message);
    if (body != NULL && *body != '\0') {
        if (content_type != NULL) {
            htttp_add_header(message, "Content-Type", content_type);
        }
        return htttp_set_body(message, body, strlen(body));
    }
    return 0;
}