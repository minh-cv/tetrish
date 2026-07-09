#ifndef TETRISH_TETRISD_EPOLLMANIP_H
#define TETRISH_TETRISD_EPOLLMANIP_H

#include "client.h"
#include "tetrissh.h"

void close_client(int epoll_fd, struct client *c);
struct client* add_client(int epoll_fd, int client_fd);
int handle_read(int epoll_fd, struct client *c, TetrishCredential* credential);
int handle_write(int epoll_fd, struct client *c);

#endif