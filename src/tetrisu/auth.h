#ifndef TETRISH_TETRISU_AUTH_H
#define TETRISH_TETRISU_AUTH_H

#include "tetrissh.h"
#include "type.h"

/*!
    @brief Post-handshake crypto only: the session key arrives from Connector,
    so this is tetrisd's auth layer minus the handshake branch and minus the
    credential.

    Keeping it as a layer, rather than folding the two session calls into
    ServerIo, is what leaves room for a non-blocking client handshake later:
    that needs a state machine and an outbound path, and both belong here.
*/
typedef struct {
    SessionKey key;
    AuthFrameQueue decrypt_q;    // inbound plaintext, owned here
    WriterFrameQueue encrypt_q;  // outbound plaintext staged by the HTTTP layer
} AuthData;

int AuthData_init(AuthData* data, const unsigned char key[SESSION_KEY_LEN], size_t queue_capacity);
void AuthData_free(AuthData* data);

/*!
    @pre the HTTTP layer has already been reset, since its parsed messages are
         non-owning views into @c decrypt_q
*/
void AuthData_reset(AuthData* data);

/*!
    @brief decrypt every frame in @p m_read_q into @p m_decrypt_q

    @post a frame whose ReaderFrameStatus is not OK is forwarded with the
          matching FrameStatus and is not decrypted
    @post a decryption failure is in-band as FRAME_DECRYPT_ERROR; the
          application layer decides what to do with it
    @post an allocation failure sets @p fault to FAULT_LOCAL
*/
void AuthData_decrypt(AuthData* data, const ReaderFrameQueue* m_read_q,
                      AuthFrameQueue* m_decrypt_q, ClientFault* fault);

/*!
    @brief encrypt every frame in @p m_encrypt_q into @p m_write_q

    @post frames appended to @p m_write_q are owned by ServerIo, which frees
          them once flushed
    @post @p m_encrypt_q is drained, whether or not every frame made it
    @post any failure sets @p fault to FAULT_TRANSPORT
*/
void AuthData_encrypt(AuthData* data, WriterFrameQueue* m_encrypt_q,
                      WriterFrameQueue* m_write_q, ClientFault* fault);

#endif
