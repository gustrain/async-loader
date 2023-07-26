/* MIT License

   Copyright (c) 2023 Gus Waldspurger

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
   */

#ifndef __ASYNC_LOADER_MODULE_H_
#define __ASYNC_LOADER_MODULE_H_

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <liburing.h>

#define MAX_PATH_LEN (128)

/* Queue entry. */
typedef struct queue_entry {
    char          path[MAX_PATH_LEN+1];     /* Filepath data was read from. */
    int           fd;                       /* File descriptor for file being
                                               loaded. Belongs to the loader.
                                               Not to be touched by workers. */
    struct iovec *iovecs;                   /* Array of iovecs for readv. */
    size_t        n_vecs;                   /* Number of structs in IOVECS. */
    size_t        size;                     /* Size of file data in bytes. */
    char          shm_fp[MAX_PATH_LEN+2];   /* Name used for shm object. */
    int           shm_lfd;                  /* File descriptor of shm object for
                                               the loader process. */
    int           shm_wfd;                  /* File descriptor of shm object for
                                               the worker process.*/
    uint8_t      *shm_ldata;                /* File data (SIZE bytes) in the shm
                                               object, accessible by the loader
                                               process. */
    uint8_t      *shm_wdata;                /* File data (SIZE bytes) in the shm
                                               object, accessible by the worker
                                               process. */
    bool          shm_lmapped;              /* If set when the entry is accessed
                                               in the free list, the loader must
                                               unmap SHM_DATA. */

    /* Free/ready link list. */
    struct worker_state *worker;            /* Worker that owns this queue. */
    struct queue_entry  *next;              /* Next entry in status list. */
    struct queue_entry  *prev;              /* Previous entry in status list. */
} entry_t;

/* Worker state. Input/output queues unique to that worker. */
typedef struct worker_state {
    /* Input buffer. */
    size_t   capacity;  /* Total number of entries in QUEUE. */
    entry_t *queue;     /* CAPACITY queue entries. */

    /* Status lists. Mutually exclusive, all using NEXT field of entry. Entries
       move exclusively in a loop, and are only ever present in at most 1 list.

            free -> ready -> completed -> free
        
       Lists should be maintained in FIFO order, and must be looped so that the
       head's PREV field points to the tail of the list. When an entry has IO
       issued, it is removed from the ready list. It is only added to the
       completed list once that IO has completed. In the interim it is tracked
       only by the uring buffer. Similarly, once a worker reads an entry from
       the completed list, it is only added to the free list upon release. */
    entry_t *free;          /* Unused queue entries. */
    entry_t *ready;         /* Queue entries ready to have IO issued. */
    entry_t *completed;     /* Queue entries with completed IO. */

    /* Synchronization. */
    pthread_spinlock_t free_lock;       /* Protects FREE. */
    pthread_spinlock_t ready_lock;      /* Protects READY. */
    pthread_spinlock_t completed_lock;  /* Protects COMPLETED. */
} wstate_t;

/* Loader (reader + responder) state. */
typedef struct {
    wstate_t       *states;         /* N_STATES worker states. */
    size_t          n_states;       /* Worker states in STATES. */
    size_t          dispatch_n;     /* Async IO requests to send at once. */
    size_t          total_size;     /* Total memory allocated. For clean up. */
    struct io_uring ring;           /* Submission ring buffer for liburing. */
} lstate_t;


bool async_try_request(wstate_t *state, char *path);
entry_t *async_try_get(wstate_t *state);
void async_release(entry_t *e);

void async_start(lstate_t *loader);
int async_init(lstate_t *loader, size_t queue_depth, size_t max_file_size, size_t n_workers, size_t min_dispatch_n);


#endif
