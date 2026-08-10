#ifndef TETRISH_TETRISD_PLAYER_IO_H
#define TETRISH_TETRISD_PLAYER_IO_H

#include "network/writer.h"
#include "type.h"

typedef struct {
    Reader reader;
    Writer writer;
} PlayerIoEntry;

#define SPARSE_SET_ELEM_TYPE PlayerIoEntry
#define SPARSE_SET_TYPEDEF SparseSet_PlayerIoEntry
#include "collection/sparse_set.h"

/*!
    @invariant A key @c fd is in @c entries if and only if the associated @c reader and @c writer are initialized, and the associated @c read_qs and @c write_qs (collectively called @c *_qs ) slots are initialized.

    @invariant Every entry in @c players_reading and @c players_writing is in @c entries .

    @invariant Every entry in @c *_qs is active iff the underlying queue has size of at least 1.
    
    @see PlayerIo_accept()
    @see PlayerIo_close()
*/
typedef struct {
    SparseSet_PlayerIoEntry entries;
    SparseSet_WriterFrameQueue write_qs;
    SparseSet_ReaderFrameQueue read_qs;
    Vec_Fd players_reading;
    Vec_Fd players_writing;
    Vec_WriterQueueStatusEntry vec_write_qs_status;
} PlayerIo;

/*!
    @brief allocate memory to members of @p data
    
    @pre @p data is not initialized

    @post All collection members (which are all members) have capacity @p max_entries and size `0`.
    @post @c *_qs has all elements uninitialized. 
    @post All elements of @c entries are uninitialized.
*/
int PlayerIo_init(PlayerIo* data, size_t max_entries);

/*!
    @brief release all memory in @p data

    @pre @p data has not been freed

    @post All frames in @c *_qs are freed
    @post All initialized entries in @c entries have readers/writers freed
    @post All initialized entries in @c *_qs and @c entries are uninitialized
    @post All collection members (which are all members) are freed
*/
void PlayerIo_free(PlayerIo* data);

/*!
    @brief reset the per-loop state for next iteration

    @post All entries in @c read_qs is inactive, with all frames in each entry freed. 
    @post @c players_reading is emptied
    @post @c players_writing is emptied
    @post @c vec_write_qs_status is emptied
    @post @c write_qs and @c entries does not change
*/
void PlayerIo_reset(PlayerIo* data);

/*!
    @brief initialize entries for each entry in @p fds , appending failed entries in @p err_fds

    @pre entries in @p fds exist neither in @c entries nor @p err_fds

    @post successful entries in @p fds has their slot in @c *_qs initialized and in inactive state. Those slot has queue capacity @p queue_capacity and size 0.
    @post successful entries in @p fds is in @c entries with @c reader and @c writer initialized and in active state.
    @post failed entries in @p fds are appended to @p err_fds
    @post @c players_reading and @c players_writing do not change

    @note if an entry in @p fds appear in @c entries or @c err_fds , that entry is ignored. This is not part of the contract.
*/
void PlayerIo_accept(
    PlayerIo* data,
    const Vec_Fd* fds,
    SparseSet_bool* err_fds,
    size_t queue_capacity
);

/*!
    @brief remove entries in @c entries for each entry in @p close_fds

    @pre the entries in @p close_fds must exist in @c entries

    @post the slot in @p close_fds is uninitialized in @c entries . Their reader/writer is freed along with, if exist, all frames in their slot of @c *_qs . Those slots are also uninitialized.

    @note if an entry does not exist in @c entries , it is ignored. This is not part of the contract.
*/
void PlayerIo_close(
    PlayerIo* data,
    const SparseSet_bool* close_fds
);

/*!
    @brief Performs one read pass over every fd in @p m_players_reading ,  
           appending newly read frames in its slot in @p m_read_qs and
           marking fds that should be closed in @p err_fds on socket error.

    @pre  No entry of @p m_players_reading is already marked in @p err_fds 

    @post For each failed fd in @p m_players_reading , it is newly
          marked in @p err_fds with its slot in @p m_read_qs inactive. Pre-existing entries of @p err_fds are preserved.
    @post Entry in @p m_read_qs is active iff its queue has size of at least 1. 

    @note If the precondition is violated, the overlapping entries are
          currently skipped. This behavior is not part of the contract
          and must not be relied upon.
*/
void PlayerIo_read(
    PlayerIo* data,
    const Vec_Fd* m_players_reading,
    SparseSet_ReaderFrameQueue* m_read_qs,
    SparseSet_bool* err_fds
);    

/*!
    @brief Ignoring failed fds, drain frames in each slot in @p m_write_qs of @c fd in @p m_players_writing to write, and/or append it to @p err_fds on socket error. Also output the current write queue status list.
    
    @pre Each entry in @p m_players_writing must have an active slot in @p m_write_qs with queue size larger than `0`.
    
    @post For each failed fd in @p m_players_writing , its slot in @p m_write_qs is inactive, along with frames there freed.
    @post After the function call, for each entry @c fd , it is not in @p err_fds but in @p m_players_writing or active in @p m_write_qs iff there is exactly one entry with that @c fd appended in @p m_vec_write_qs_status (zero otherwise). The size of the queue determines the corresponding status.
    @post Entry in @p m_write_qs is active iff its queue has size of at least 1. 

    @note If the first precondition is violated, the overlapping entries are
        currently skipped. This behavior is not part of the contract
        and must not be relied upon.
*/        

void PlayerIo_write(
    PlayerIo* data,
    SparseSet_WriterFrameQueue* m_write_qs,
    const Vec_Fd* m_players_writing,
    SparseSet_bool* err_fds,
    Vec_WriterQueueStatusEntry* m_vec_write_qs_status
);

#endif
