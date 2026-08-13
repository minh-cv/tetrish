#ifndef TETRISH_TETRISU_NET_TETRISSH_CHANNEL_H
#define TETRISH_TETRISU_NET_TETRISSH_CHANNEL_H

#include "net/error.h"
#include "net/message.h"
#include "net/socket_transport.h"

#include "tetrissh.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SECURE_CHANNEL_NEW,
    SECURE_CHANNEL_SEND_NONCE,
    SECURE_CHANNEL_RECV_PROOF,
    SECURE_CHANNEL_RECV_CERTIFICATE,
    SECURE_CHANNEL_SEND_SESSION_KEY,
    SECURE_CHANNEL_READY,
    SECURE_CHANNEL_FAILED,
} SecureChannelState;

enum {
    SECURE_CHANNEL_WANT_READ = 1u << 0,
    SECURE_CHANNEL_WANT_WRITE = 1u << 1,
};

enum {
    SECURE_CHANNEL_EVENT_NONE = 0,
    SECURE_CHANNEL_EVENT_HANDSHAKE_READY = 1u << 0,
    SECURE_CHANNEL_EVENT_APP_SENT = 1u << 1,
    SECURE_CHANNEL_EVENT_PLAINTEXT = 1u << 2,
    SECURE_CHANNEL_EVENT_CLOSED = 1u << 3,
    SECURE_CHANNEL_EVENT_ERROR = 1u << 4,
};

typedef struct {
    unsigned events;
    OwnedBytes plaintext;
    ClientError error;
} SecureChannelStep;

/*!
    @invariant at most one input frame and one output frame are partially owned
    @invariant the session key is usable only in `SECURE_CHANNEL_READY`
    @invariant queued application output does not disable authenticated input
*/
typedef struct {
    SecureChannelState state;
    const char* ca_path;
    unsigned char nonce[NONCE_LEN];
    SessionKey key;
    OwnedBytes proof;
    uint8_t input_length[sizeof(uint32_t)];
    size_t input_length_used;
    OwnedBytes input_body;
    size_t input_body_used;
    OwnedBytes output;
    size_t output_used;
    unsigned output_kind;
} TetrisshChannel;

/*!
    @brief initialize an incremental tetrissh client channel

    @pre @p channel is not initialized
    @pre @p ca_path is a NUL-terminated path that remains valid until channel free
    @post @p channel owns no allocation and is in `SECURE_CHANNEL_NEW`
*/
void tetrissh_channel_init(TetrisshChannel* channel, const char* ca_path);

/*!
    @brief release frame buffers and erase the session key
    @pre @p channel is initialized and has not been freed
    @post @p channel owns no allocation and returns to `SECURE_CHANNEL_NEW`
*/
void tetrissh_channel_free(TetrisshChannel* channel);

/*!
    @brief begin the client handshake by queuing a freshly generated nonce

    @pre @p channel is in `SECURE_CHANNEL_NEW`
    @post on success, a nonce frame is queued and write interest is enabled
    @post on failure, @p channel is failed and @p error describes the failure

    @return `0` on success, `-1` on random-source or allocation failure
*/
int tetrissh_channel_start(TetrisshChannel* channel, ClientError* error);

/*!
    @brief return the poll interests needed to advance the channel
    @pre @p channel is initialized
    @post @p channel is unchanged
*/
unsigned tetrissh_channel_want(const TetrisshChannel* channel);

/*!
    @brief encrypt and queue one authenticated application message

    @pre @p channel is ready and has no queued output
    @pre @p plaintext points to @p length readable bytes when length is nonzero
    @post on success, one encrypted length-prefixed frame is queued
    @post @p plaintext remains owned by the caller

    @return `0` on success, `-1` when the message cannot be encrypted or queued
*/
int tetrissh_channel_submit(
    TetrisshChannel* channel,
    const unsigned char* plaintext,
    size_t length,
    ClientError* error
);

/*!
    @brief perform bounded non-blocking channel work for ready poll conditions

    @pre @p channel is initialized and @p transport owns a connected non-blocking fd
    @pre @p step has no owned plaintext
    @pre @p readable and @p writable reflect the most recent poll result
    @post the function never waits for additional socket input or output capacity
    @post on `PLAINTEXT`, @p step owns exactly one decrypted application frame
    @post on `ERROR`, @p channel is failed and @p step describes the failure

    @return `0` after a successful pass, `-1` iff `ERROR` is emitted
*/
int tetrissh_channel_step(
    TetrisshChannel* channel,
    SocketTransport* transport,
    bool readable,
    bool writable,
    SecureChannelStep* step
);

/*!
    @brief release plaintext owned by a channel step and reset its event fields
    @post @p step owns no allocation
*/
void tetrissh_channel_step_free(SecureChannelStep* step);

#endif
