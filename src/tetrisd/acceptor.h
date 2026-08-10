#ifndef TETRISH_TETRISD_ACCEPTOR_H
#define TETRISH_TETRISD_ACCEPTOR_H

#include "type.h"

typedef struct {
    int listen_fd;
    Vec_Fd accepted;
} Acceptor;

int Acceptor_init(Acceptor* data, const char* address, int port, size_t max_entries);
void Acceptor_free(Acceptor* data);
void Acceptor_reset(Acceptor* data);

/*
    accept(2) until EAGAIN, pushing successful fds to m_accepted_out.
    should_stop_accepting is set when no more connections can be admitted
    until a slot frees: the fd table is full (m_fd_limit) or the process/system
    fd limit is exhausted (EMFILE/ENFILE).
*/
void Acceptor_accept(
    Acceptor* data,
    size_t m_fd_limit,
    Vec_Fd* m_accepted_out,
    bool* should_stop_accepting
);

#endif
