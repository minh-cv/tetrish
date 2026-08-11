#ifndef TETRISH_TETRISD_HTTTP_LAYER_H
#define TETRISH_TETRISD_HTTTP_LAYER_H

#include "type.h"

/*!
    @brief The application step between decrypt and encrypt: parse each
    decrypted frame in @p m_decrypt_qs as an HTTTP request and append the
    serialized default response to its slot in @p m_auth_qs , marking fds
    whose response could not be built in @p err_fds .

    Frames carrying an in-band error status (decrypt failure, oversized
    read) are forwarded into @p m_auth_qs unchanged, leaving the close
    decision to encrypt, the last layer before the socket. A frame that is
    sound at the transport level but is not a valid HTTTP request is
    answered with a 400 response instead; it is not an error. A response
    whose echoed body would overflow a frame is downgraded to a bodyless
    413.

    This is a single-purpose component: parsing is per-frame and the
    default response carries no session state, so it owns no entries and
    has no accept/close/reset. Both queues it touches are owned by
    AuthData; responses appended to @p m_auth_qs are freed by
    AuthData_reset like any other frame there.

    @pre No entry of @p m_decrypt_qs is already marked in @p err_fds

    @post For each failed fd in @p m_decrypt_qs , it is newly marked in
          @p err_fds with its slot in @p m_auth_qs inactive, along with
          frames there freed. Pre-existing entries of @p err_fds are
          preserved.
    @post Entry in @p m_auth_qs is active iff its queue has size of at least 1.
    @post Frame contents in @p m_decrypt_qs may be modified in place
          (parsing splits them into strings); their ownership does not
          change.

    @note If the precondition is violated, the overlapping entries are
          currently skipped. This behavior is not part of the contract
          and must not be relied upon.
*/
void Htttp_respond(
    const SparseSet_AuthFrameQueue* m_decrypt_qs,
    SparseSet_AuthFrameQueue* m_auth_qs,
    SparseSet_bool* err_fds
);

#endif
