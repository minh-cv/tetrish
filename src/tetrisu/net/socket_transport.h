#ifndef TETRISH_TETRISU_NET_SOCKET_TRANSPORT_H
#define TETRISH_TETRISU_NET_SOCKET_TRANSPORT_H

#include "net/error.h"

#include <stddef.h>
#include <sys/types.h>

typedef enum {
    SOCKET_CONNECT_FAILED = -1,
    SOCKET_CONNECT_IN_PROGRESS = 0,
    SOCKET_CONNECT_CONNECTED = 1,
} SocketConnectResult;

typedef struct {
    int fd;
} SocketTransport;

/*!
    @brief initialize an empty socket transport
    @pre @p transport is not initialized
    @post @p transport owns no descriptor and has fd `-1`
*/
void socket_transport_init(SocketTransport* transport);

/*!
    @brief close the owned descriptor, if any, and restore the empty state
    @pre @p transport is initialized
    @post @p transport owns no descriptor and has fd `-1`
*/
void socket_transport_reset(SocketTransport* transport);

/*!
    @brief resolve an endpoint and start a non-blocking TCP connection

    @pre @p transport is initialized and has fd `-1`
    @pre @p address is a NUL-terminated host name and @p port is in `(0, 65535]`
    @post on connected/in-progress, @p transport owns one non-blocking descriptor
    @post on failure, @p transport remains empty and @p error describes the failure

    @return the connection status
*/
SocketConnectResult socket_transport_connect_start(
    SocketTransport* transport,
    const char* address,
    int port,
    ClientError* error
);

/*!
    @brief finish a connection after poll reports the descriptor writable

    @pre @p transport owns a connection-in-progress descriptor
    @post on success, the descriptor is connected and remains owned by @p transport
    @post on failure, ownership is unchanged and @p error describes the socket error

    @return `1` if connected, `-1` on failure
*/
int socket_transport_connect_finish(SocketTransport* transport, ClientError* error);

/*!
    @brief receive bytes without blocking
    @pre @p transport owns a connected non-blocking descriptor
    @pre @p buffer points to @p capacity writable bytes when capacity is nonzero
    @post returns bytes read, `0` for peer close, or `-1` with errno preserved
*/
ssize_t socket_transport_read(SocketTransport* transport, void* buffer, size_t capacity);

/*!
    @brief send bytes without blocking
    @pre @p transport owns a connected non-blocking descriptor
    @pre @p buffer points to @p length readable bytes when length is nonzero
    @post returns bytes written or `-1` with errno preserved
*/
ssize_t socket_transport_write(SocketTransport* transport, const void* buffer, size_t length);

#endif
