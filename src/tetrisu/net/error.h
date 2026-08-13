#ifndef TETRISH_TETRISU_NET_ERROR_H
#define TETRISH_TETRISU_NET_ERROR_H

typedef enum {
    CLIENT_ERROR_NONE,
    CLIENT_ERROR_DNS,
    CLIENT_ERROR_CONNECT,
    CLIENT_ERROR_TIMEOUT,
    CLIENT_ERROR_HANDSHAKE,
    CLIENT_ERROR_CERTIFICATE,
    CLIENT_ERROR_TRANSPORT,
    CLIENT_ERROR_DECRYPT,
    CLIENT_ERROR_HTTTP_PARSE,
    CLIENT_ERROR_PROTOCOL,
    CLIENT_ERROR_NOMEM,
    CLIENT_ERROR_INTERNAL,
} ClientErrorDomain;

typedef struct {
    ClientErrorDomain domain;
    int code;
    const char* detail;
} ClientError;

/*!
    @brief construct a borrowed, allocation-free client error value
    @pre @p detail is NULL or points to a string that outlives every copy
    @post the returned value owns no allocation
*/
static inline ClientError client_error(
    ClientErrorDomain domain,
    int code,
    const char* detail
) {
    const ClientError error = {domain, code, detail};
    return error;
}

#endif
