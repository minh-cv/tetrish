#ifndef TETRISH_TETRISU_NET_CLIENT_H
#define TETRISH_TETRISU_NET_CLIENT_H

#include "net/event.h"
#include "net/htttp_codec.h"
#include "net/message.h"
#include "net/socket_transport.h"
#include "net/tetrissh_channel.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_CLIENT_DISCONNECTED,
    NET_CLIENT_CONNECTING,
    NET_CLIENT_HANDSHAKING,
    NET_CLIENT_READY_IDLE,
    NET_CLIENT_READY_SENDING,
    NET_CLIENT_READY_AWAITING_REPLY,
} NetClientState;

/*!
    @invariant the transport owns an fd iff state is not disconnected
    @invariant pending plaintext is nonempty exactly while sending/awaiting reply
    @invariant connect, handshake, send and awaiting-reply states have a deadline
*/
typedef struct {
    NetClientState state;
    const char* address;
    int port;
    SocketTransport transport;
    TetrisshChannel secure;
    OwnedBytes pending_plaintext;
    ClientRequestCompletion pending_completion;
    uint64_t deadline_ms;
    bool has_deadline;
} NetClient;

/*!
    @brief initialize a disconnected single-connection client

    @pre @p client is not initialized
    @pre @p address and @p ca_path are NUL-terminated and remain valid until free
    @pre @p port is in `(0, 65535]`
    @post @p client owns no descriptor or message and is disconnected
*/
void net_client_init(NetClient* client, const char* address, int port, const char* ca_path);

/*!
    @brief close the connection and release all client-owned data
    @pre @p client is initialized and has not been freed
    @post @p client owns no descriptor or allocation and is disconnected
*/
void net_client_free(NetClient* client);

/*!
    @brief start a new non-blocking connection and handshake

    @pre @p client is initialized
    @pre @p events is initialized and empty
    @post any previous connection and pending request are discarded
    @post on success, `CONNECTING` is emitted and poll interest is available
    @post on failure, `ERROR` is emitted and the client is disconnected

    @return `0` if the action was represented by events, `-1` only if an event
            cannot be appended
*/
int net_client_connect(NetClient* client, uint64_t now_ms, NetEventList* events);

/*!
    @brief explicitly close the connection and cancel any pending request
    @pre @p client and @p events are initialized
    @post @p client is disconnected and `DISCONNECTED` is appended
*/
int net_client_disconnect(NetClient* client, NetEventList* events);

/*!
    @brief encode and queue one HTTTP request

    The HTTTP encoder is reachable only through this operation, and this
    operation is accepted only in `NET_CLIENT_READY_IDLE`. The serialized
    plaintext is retained until a terminal response or exact legacy echo.

    @pre @p client and @p events are initialized
    @pre @p payload points to @p length readable bytes when length is nonzero
    @post on acceptance, one encrypted frame is queued, `SEND_ACCEPTED` is
          emitted, and the client is no longer idle
    @post on state/payload failure, `ERROR` is emitted and the connection closes

    @return `0` if the action was represented by events, `-1` only if an event
            cannot be appended
*/
int net_client_send(
    NetClient* client,
    const unsigned char* payload,
    size_t length,
    uint64_t now_ms,
    NetEventList* events
);

/*!
    @brief encode and queue one typed HTTTP request

    @pre @p client is in `NET_CLIENT_READY_IDLE`
    @pre fields borrowed by @p request remain valid for this call
    @pre @p events is initialized and empty
    @post on acceptance, serialized plaintext is retained until its configured
          completion point and `SEND_ACCEPTED` is appended
    @post an `EXPECT_REPLY` request enters `READY_AWAITING_REPLY` after write
    @post a `COMPLETE_ON_SEND` request returns to `READY_IDLE` after write and
          appends `SEND_COMPLETED`
    @post @p request and its body remain owned by the caller

    @return `0` if represented by events, `-1` only if an event cannot append
*/
int net_client_send_request(
    NetClient* client,
    const ClientRequest* request,
    uint64_t now_ms,
    NetEventList* events
);

/*!
    @brief return the currently owned socket descriptor, or `-1`
    @pre @p client is initialized
    @post @p client is unchanged
*/
int net_client_fd(const NetClient* client);

/*!
    @brief return `POLLIN`/`POLLOUT` interests for the current state
    @pre @p client is initialized
    @post @p client is unchanged
*/
short net_client_poll_events(const NetClient* client);

/*!
    @brief advance connection, handshake, framing, decode, and dispatch

    Every complete authenticated plaintext frame in an authenticated state is
    passed exactly once to the HTTTP decoder. `STATE` requests emit
    `STATE_PUSH` in idle, sending, or awaiting-reply states and never complete
    the pending request. Responses and exact legacy echoes are accepted only
    while awaiting a reply. Other server requests are rejected and disconnect.

    @pre @p revents came from polling `net_client_fd()`
    @pre @p events is initialized and empty
    @post no socket operation waits for readiness not present in @p revents
    @post emitted events own their payloads

    @return `0` if the readiness was represented by events, `-1` only if an
            event cannot be appended
*/
int net_client_on_poll(
    NetClient* client,
    short revents,
    uint64_t now_ms,
    NetEventList* events
);

/*!
    @brief return milliseconds until the next client deadline
    @pre @p client is initialized
    @post @p client is unchanged
    @return `-1` when no deadline exists, otherwise a value in `[0, INT_MAX]`
*/
int net_client_timeout_ms(const NetClient* client, uint64_t now_ms);

/*!
    @brief expire the active connect, handshake, or reply deadline if due
    @pre @p client and @p events are initialized
    @post if due, the client disconnects and emits `ERROR`
    @return `0` if represented by events, `-1` only if an event cannot append
*/
int net_client_on_timeout(NetClient* client, uint64_t now_ms, NetEventList* events);

#endif
