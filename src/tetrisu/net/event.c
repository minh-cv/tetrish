#include "net/event.h"

#include <string.h>

void net_event_list_init(NetEventList* list) {
    memset(list, 0, sizeof(*list));
}

int net_event_list_push(NetEventList* list, NetEvent* event) {
    if (list->count == NET_EVENT_LIST_CAPACITY) {
        return -1;
    }
    list->items[list->count] = *event;
    owned_bytes_init(&event->payload);
    ++list->count;
    return 0;
}

void net_event_list_free(NetEventList* list) {
    for (size_t i = 0; i < list->count; ++i) {
        owned_bytes_free(&list->items[i].payload);
    }
    memset(list, 0, sizeof(*list));
}
