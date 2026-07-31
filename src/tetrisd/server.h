#ifndef TETRISH_TETRISD_SERVER_H
#define TETRISH_TETRISD_SERVER_H
#include "type.h"

#define SPAN_ELEM_TYPE struct epoll_event
#define SPAN_TYPEDEF EpollEvents
#include "collection/span.h"

void server_free(Server* server);
int server_init(Server* server);
void server_reload_config(Server* server);
int server_poll(Server* server, EpollEvents* evs);

#endif
