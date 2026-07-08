#ifndef TETRISH_HTTTP_H
#define TETRISH_HTTTP_H

#include <stddef.h>

#define HTTTP_MAX_MESSAGE (64U * 1024U)
#define HTTTP_MAX_HEADERS 32
#define HTTTP_MAX_METHOD 16
#define HTTTP_MAX_PATH 256
#define HTTTP_MAX_REASON 64
#define HTTTP_MAX_HEADER_NAME 64
#define HTTTP_MAX_HEADER_VALUE 256

typedef enum {
    HTTTP_REQUEST = 1,
    HTTTP_RESPONSE = 2
} htttp_kind_t;

typedef struct {
    char name[HTTTP_MAX_HEADER_NAME];
    char value[HTTTP_MAX_HEADER_VALUE];
} htttp_header_t;

typedef struct {
    htttp_kind_t kind;
    char method[HTTTP_MAX_METHOD];
    char path[HTTTP_MAX_PATH];
    int status;
    char reason[HTTTP_MAX_REASON];
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t header_count;
    unsigned char *body;
    size_t body_length;
} htttp_message_t;

void htttp_message_init(htttp_message_t *message);
void htttp_message_free(htttp_message_t *message);
int htttp_add_header(htttp_message_t *message, const char *name, const char *value);
const char *htttp_get_header(const htttp_message_t *message, const char *name);
int htttp_set_body(htttp_message_t *message, const void *body, size_t length);
int htttp_parse(const unsigned char *buffer, size_t length, htttp_message_t *message);
int htttp_serialize(const htttp_message_t *message, unsigned char **buffer, size_t *length);
int htttp_make_response(htttp_message_t *message, int status, const char *reason,
                        const char *body, const char *content_type);

#endif

