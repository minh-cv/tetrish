#include "net/htttp_codec.h"

#include "wire.h"

#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool field_is_safe(const char* field) {
    return field != NULL && strchr(field, '\r') == NULL && strchr(field, '\n') == NULL;
}

int htttp_codec_encode_request(const ClientRequest* request, OwnedBytes* out) {
    owned_bytes_init(out);
    if (request == NULL ||
        !field_is_safe(request->method) ||
        !field_is_safe(request->path) ||
        !field_is_safe(request->content_type) ||
        (request->body_len != 0 && request->body == NULL)) {
        return -1;
    }

    char content_length[32];
    const int length_written = snprintf(
        content_length,
        sizeof(content_length),
        "%zu",
        request->body_len
    );
    if (length_written < 0 || (size_t)length_written >= sizeof(content_length)) {
        return -1;
    }

    const HtttpMessage message = {
        .request = {
            .method = request->method,
            .path = request->path,
            .header = {
                {"Content-Length", content_length},
                {"Content-Type", request->content_type},
            },
            .header_count = 2,
            .body = request->body,
            .body_len = request->body_len,
        },
        .is_request = true,
    };

    size_t serialized_len = 0;
    unsigned char* serialized = htttp_serialize(&message, &serialized_len);
    if (serialized == NULL || serialized_len > FRAME_MAX || serialized_len > UINT32_MAX) {
        free(serialized);
        return -1;
    }

    out->ptr = serialized;
    out->len = serialized_len;
    return 0;
}

int htttp_codec_decode_owned(OwnedBytes* backing, OwnedHtttpMessage* out) {
    memset(out, 0, sizeof(*out));
    OwnedBytes moved;
    owned_bytes_init(&moved);
    owned_bytes_move(&moved, backing);

    if (moved.ptr == NULL || moved.len == 0 ||
        htttp_parse(moved.ptr, moved.len, &out->view) == -1) {
        owned_bytes_free(&moved);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    owned_bytes_move(&out->backing, &moved);
    return 0;
}

bool htttp_codec_is_state_push(const OwnedHtttpMessage* message) {
    if (!message->view.is_request ||
        strcmp(message->view.request.method, "STATE") != 0) {
        return false;
    }
    const char* content_type = htttp_get_header(&message->view, "Content-Type");
    if (content_type == NULL ||
        strcmp(content_type, "application/tetris-state") != 0) {
        return false;
    }
    static const char prefix[] = "/room/";
    const char* path = message->view.request.path;
    if (path == NULL || strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    path += sizeof(prefix) - 1;
    if (*path == '\0') {
        return false;
    }
    for (; *path != '\0'; ++path) {
        if (!isdigit((unsigned char)*path)) {
            return false;
        }
    }
    return true;
}

OwnedBytes htttp_codec_borrow_body(const OwnedHtttpMessage* message) {
    OwnedBytes body;
    if (message->view.is_request) {
        body.ptr = (unsigned char*)message->view.request.body;
        body.len = message->view.request.body_len;
    } else {
        body.ptr = (unsigned char*)message->view.response.body;
        body.len = message->view.response.body_len;
    }
    return body;
}

void owned_htttp_message_free(OwnedHtttpMessage* message) {
    owned_bytes_free(&message->backing);
    memset(&message->view, 0, sizeof(message->view));
}
