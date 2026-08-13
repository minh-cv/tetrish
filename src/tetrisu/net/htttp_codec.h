#ifndef TETRISH_TETRISU_NET_HTTTP_CODEC_H
#define TETRISH_TETRISU_NET_HTTTP_CODEC_H

#include "htttp.h"
#include "net/message.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char* method;
    const char* path;
    const unsigned char* body;
    size_t body_len;
    const char* content_type;
    ClientRequestCompletion completion;
} ClientRequest;

typedef struct {
    OwnedBytes backing;
    HtttpMessage view;
} OwnedHtttpMessage;

/*!
    @brief serialize one client request into owned plaintext bytes

    @pre @p request and @p out are non-NULL
    @pre @p out owns no allocation
    @pre method, path and content type are NUL-terminated and contain no CR/LF
    @pre body is non-NULL when body_len is nonzero

    @post on success, @p out uniquely owns the serialized HTTTP message
    @post on failure, @p out remains empty

    @return `0` on success, `-1` on invalid input, frame-limit/size overflow,
            or allocation failure
*/
int htttp_codec_encode_request(const ClientRequest* request, OwnedBytes* out);

/*!
    @brief destructively parse owned plaintext bytes as an HTTTP message

    @pre @p backing uniquely owns its allocation
    @pre @p out owns no allocation

    @post on success, ownership moves from @p backing to @p out and every
          pointer in @c out->view borrows @c out->backing
    @post on failure, @p backing is freed and empty and @p out remains empty

    @return `0` on success, `-1` for invalid HTTTP
*/
int htttp_codec_decode_owned(OwnedBytes* backing, OwnedHtttpMessage* out);

/*!
    @brief report whether @p message is a typed one-way `STATE /room/<fd>` request

    @pre @p message is a successfully decoded message
    @post @p message is unchanged

    The message must carry Content-Type `application/tetris-state` and a room
    path whose suffix consists only of decimal digits.
*/
bool htttp_codec_is_state_push(const OwnedHtttpMessage* message);

/*!
    @brief return the borrowed body slice of @p message

    @pre @p message is a successfully decoded message
    @post the returned slice remains valid until @p message is freed
*/
OwnedBytes htttp_codec_borrow_body(const OwnedHtttpMessage* message);

/*!
    @brief free @p message's backing allocation

    @post @p message is empty and may be freed again
*/
void owned_htttp_message_free(OwnedHtttpMessage* message);

#endif
