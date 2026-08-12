#ifndef TETRISH_TETRISD_SOCKET_H
#define TETRISH_TETRISD_SOCKET_H

int prepare_socket(const char* address, int port);
int prepare_logger_socket(const char* log_ipc);
int set_nonblocking(int fd);

#endif
