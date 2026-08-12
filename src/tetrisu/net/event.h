#ifndef TETRISH_TETRISU_NET_EVENT_H
#define TETRISH_TETRISU_NET_EVENT_H

#include "net/error.h"
#include "net/message.h"

#include <stddef.h>

#define NET_EVENT_LIST_CAPACITY 16u

typedef enum {
    NET_EVENT_CONNECTING,
    NET_EVENT_HANDSHAKING,
    NET_EVENT_CONNECTED,
    NET_EVENT_SEND_ACCEPTED,
    NET_EVENT_REPLY,
    NET_EVENT_ECHO,
    NET_EVENT_STATE_PUSH,
    NET_EVENT_DISCONNECTED,
    NET_EVENT_ERROR,
} NetEventType;

typedef struct {
    NetEventType type;
    OwnedBytes payload;
    ClientError error;
} NetEvent;

typedef struct {
    NetEvent items[NET_EVENT_LIST_CAPACITY];
    size_t count;
} NetEventList;

/*!
    @brief initialize an empty event list
    @post @p list owns no payload and has count `0`
*/
void net_event_list_init(NetEventList* list);

/*!
    @brief append @p event by moving its owned payload

    @pre @p list has fewer than @c NET_EVENT_LIST_CAPACITY entries
    @post on success, @p event owns no payload and list count increases by one
    @post on failure, both arguments are unchanged

    @return `0` on success, `-1` when the fixed-capacity list is full
*/
int net_event_list_push(NetEventList* list, NetEvent* event);

/*!
    @brief release all payloads in @p list
    @post @p list has count `0` and may be freed again
*/
void net_event_list_free(NetEventList* list);

#endif
