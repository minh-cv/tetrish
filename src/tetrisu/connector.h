#ifndef TETRISH_TETRISU_CONNECTOR_H
#define TETRISH_TETRISU_CONNECTOR_H

#include "tetrissh.h"

/*!
    @brief The client's counterpart to tetrisd's Acceptor: it produces the one
    descriptor everything else in the client is about, and owns it.

    Resolution, connect and the tetrissh handshake all happen inside
    @c Connector_init against a *blocking* socket, because corestack's
    @c tetrish_client_handshake blocks; only afterwards is the descriptor
    switched to non-blocking and handed to the I/O and auth layers. That is
    sound at startup, where there is nothing else for the client to do, and is
    the reason in-session reconnect is out of scope.
*/
typedef struct {
    int server_fd;      // -1 when not connected
    SessionKey key;     // valid iff server_fd >= 0
} Connector;

/*!
    @brief resolve @p address : @p port , connect, and complete the tetrissh
    handshake against @p ca_path , then put the socket in non-blocking mode.

    @post on success @c server_fd is a connected non-blocking socket and
          @c key holds the negotiated session key
    @return -1 on any failure, with no descriptor leaked
*/
int Connector_init(Connector* data, const char* address, int port, const char* ca_path);

void Connector_free(Connector* data);

#endif
