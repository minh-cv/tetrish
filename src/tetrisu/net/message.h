#ifndef TETRISH_TETRISU_NET_MESSAGE_H
#define TETRISH_TETRISU_NET_MESSAGE_H

#include <stddef.h>

/*!
    @brief How a successfully transmitted client request completes.

    Room lifecycle requests receive an HTTTP response. Gameplay inputs are
    intentionally one-way and complete as soon as their encrypted frame has
    been fully written.
*/
typedef enum {
    CLIENT_REQUEST_EXPECT_REPLY,
    CLIENT_REQUEST_COMPLETE_ON_SEND,
} ClientRequestCompletion;

typedef struct {
    unsigned char* ptr;
    size_t len;
} OwnedBytes;

/*!
    @brief reset @p bytes to the empty non-owning value

    @post @p bytes owns no allocation and has length `0`
*/
void owned_bytes_init(OwnedBytes* bytes);

/*!
    @brief copy @p len bytes from @p source into @p out

    @pre @p out owns no allocation
    @pre @p source is non-NULL when @p len is nonzero

    @post on success, @p out uniquely owns a byte-for-byte copy
    @post on failure, @p out remains empty

    @return `0` on success, `-1` on allocation failure
*/
int owned_bytes_copy(OwnedBytes* out, const void* source, size_t len);

/*!
    @brief transfer the allocation owned by @p source to @p destination

    @pre @p destination owns no allocation

    @post @p destination has the pre-call value of @p source
    @post @p source is empty
*/
void owned_bytes_move(OwnedBytes* destination, OwnedBytes* source);

/*!
    @brief free the allocation owned by @p bytes

    @post @p bytes is empty and may be freed again
*/
void owned_bytes_free(OwnedBytes* bytes);

#endif
