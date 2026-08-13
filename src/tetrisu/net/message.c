#include "net/message.h"

#include <stdlib.h>
#include <string.h>

void owned_bytes_init(OwnedBytes* bytes) {
    bytes->ptr = NULL;
    bytes->len = 0;
}

int owned_bytes_copy(OwnedBytes* out, const void* source, size_t len) {
    owned_bytes_init(out);
    if (len == 0) {
        return 0;
    }

    out->ptr = malloc(len);
    if (out->ptr == NULL) {
        return -1;
    }
    memcpy(out->ptr, source, len);
    out->len = len;
    return 0;
}

void owned_bytes_move(OwnedBytes* destination, OwnedBytes* source) {
    *destination = *source;
    owned_bytes_init(source);
}

void owned_bytes_free(OwnedBytes* bytes) {
    free(bytes->ptr);
    owned_bytes_init(bytes);
}
