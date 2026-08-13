#ifndef TETRISH_TETRISD_AUTH_H
#define TETRISH_TETRISD_AUTH_H

#include "type.h"
#include "tetrissh.h"

typedef enum {
    AUTH_NONCE,
    AUTH_SYMKEY,
    AUTH_DONE,
} AuthState;

typedef struct {
    SessionKey key;
    AuthState auth_state;
} AuthEntry;

#define SPARSE_SET_ELEM_TYPE AuthEntry
#define SPARSE_SET_TYPEDEF SparseSet_AuthEntry
#include "collection/sparse_set.h"

/*!
    @invariant A key @c fd is in @c entries iff the associated @c auth_qs and @c decrypt_qs (collectively called @c *_qs ) slots are initialized.

    @invariant Every entry in @c *_qs is active iff the underlying queue has size of at least 1.
*/
typedef struct {
    SparseSet_AuthEntry entries;
    TetrishCredential credential;
    SparseSet_WriterFrameQueue encrypt_qs;
    SparseSet_AuthFrameQueue decrypt_qs;
} AuthData;

/*!
    @brief allocate memory to members of @p data and initialize credentials
    
    @pre @p data is not initialized

    @post All collection members have capacity @p max_entries and size `0`.
    @post @c *_qs has all elements uninitialized. 
    @post All elements of @c entries are uninitialized.

    @return -1 if failed, 0 otherwise
*/
int AuthData_init(AuthData* data, size_t max_entries, const char* key_path, const char* certificate_path);

/*!
    @brief release all memory in @p data

    @pre @p data has not been freed

    @post All frames in @c *_qs are freed
    @post All initialized entries in @c *_qs and @c entries are uninitialized
    @post All collection members are freed
    @post @c credential is freed
*/
void AuthData_free(AuthData* data);

/*!
    @brief replace @c credential with the one at @p key_path and @p certificate_path

    Validate-then-swap: the new credential is loaded and checked in full before
    the old one is released, so a failure leaves @p data untouched.

    @pre @p data is initialized
    @pre no handshake pass is in flight (reload runs between ticks)

    @post on success the old credential is freed and @c credential is the new
          one; entries already at `AUTH_DONE` are unaffected, since their
          session keys are derived. An entry at `AUTH_SYMKEY` encrypted its
          session key to the old certificate and will fail its next decrypt,
          which fails that fd; the client reconnects.
    @post on failure @c credential is unchanged.

    @return -1 if failed, 0 otherwise
*/
int AuthData_reload_credential(AuthData* data, const char* key_path, const char* certificate_path);

/*!
    @brief reset the per-loop state for next iteration

    @post All entries in @c *_qs is inactive, with all frames in each entry freed.
    @post other members do not change
*/
void AuthData_reset(AuthData* data);

/*!
    @brief initialize entries for each entry in @p fds , appending failed entries in @p err_fds

    @pre entries in @p fds exist neither in @c entries nor @p err_fds

    @post successful entries in @p fds has their slot in @c *_qs initialized and in inactive state. Those slot has queue capacity @p queue_capacity and size 0.
    @post successful entries in @p fds is in @c entries with `auth_state == AUTH_NONCE`
    @post failed entries in @p fds are appended to @p err_fds

    @note if an entry in @p fds appear in @c entries or @c err_fds , that entry is ignored. This is not part of the contract.
*/
void AuthData_accept(
    AuthData* data,
    const Vec_Fd* fds, 
    SparseSet_bool* err_fds,
    size_t queue_capacity
);

/*!
    @brief remove entries in @c entries for each entry in @p close_fds

    @pre the entries in @p close_fds must exist in @c entries

    @post the slot in @p close_fds is uninitialized in @c entries . If exists, all frames in their slot of @c *_qs are freed. Those slots are also uninitialized.

    @note if an entry does not exist in @c entries , it is ignored. This is not part of the contract.
*/
void AuthData_close(
    AuthData* data,
    const SparseSet_bool* close_fds
);

/*!
    @brief Advance decryption or handshaking for every fd in @p read_qs ,  
           appending new handshaking frames in its slot in @p handshake_out or decrypted frames in its slot in @p m_decrypted and
           marking fds that should be closed in @p err_fds .

    For each decrypted frame, 

    @pre  No entry of @p read_qs is already marked in @p err_fds
    @pre  @p read_qs and @p m_decrypted slots were accepted with the same
          queue capacity (see server_tick's accept fan-out), so the
          one-frame-per-input output always fits.

    @post A frame with a non-OK ReaderFrameStatus is forwarded into its fd's slot in @p m_decrypted only if that fd's auth state is `AUTH_DONE`; if the handshake is not complete, the fd is failed instead.
    @post For each failed fd in @p read_qs , it is newly
          marked in @p err_fds with its slot in @p m_decrypted inactive, along with frames there freed. Pre-existing entries of @p err_fds are preserved.
    @post For each failed fd in @p read_qs , its slot in @p handshake_out is drained and inactive, regardless of whether the failure was in a handshake or a decrypt step. This function owns the fd's slot in @p handshake_out until the fd's pass commits; encrypt appends to it only afterwards, so the slot holds handshake frames only and draining it discards no other layer's output.
    @post Entry in @p m_decrypted is active iff its queue has size of at least 1.

    @note If the first precondition is violated, the overlapping entries
          are currently skipped. If the second is violated, the fd is
          currently failed like an operation failure. Neither behavior is
          part of the contract and must not be relied upon.
*/
void AuthData_handshake_or_decrypt(
    AuthData* data,
    const SparseSet_ReaderFrameQueue* read_qs,
    SparseSet_AuthFrameQueue* m_decrypted,
    SparseSet_WriterFrameQueue* handshake_out,
    SparseSet_bool* err_fds
);

/*!
    @brief Perform encryption for every fd in @p m_auth_qs ,  
           appending new encrypted frames in its slot in @p out and
           marking fds that should be closed in @p err_fds .

    @pre  No entry of @p m_auth_qs is already marked in @p err_fds 

    @post For each failed fd in @p m_auth_qs , it is newly
          marked in @p err_fds . Pre-existing entries of @p err_fds are preserved.

    @note If the precondition is violated, the overlapping entries are
          currently skipped. This behavior is not part of the contract
          and must not be relied upon.
*/
void AuthData_encrypt(
    AuthData* data,
    const SparseSet_WriterFrameQueue* m_encrypt_qs,
    SparseSet_WriterFrameQueue* out,
    SparseSet_bool* err_fds
);

#endif
