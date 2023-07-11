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
#include <stdatomic.h>
#include <linux/io_uring.h>

#define MAX_PATH_LEN (128)

#define ALLOCATED_FLAG  (0b0001)
#define IN_FLIGHT_FLAG  (0b0010)
#define PENDING_FLAG    (0b0100)
#define READY_FLAG      (0b1000)
#define NONE_FLAG       (0b0000)


/* Queue entry. */
typedef struct {
    char          path[MAX_PATH_LEN];   /* Filepath data was read from. */
    int           fd;                   /* File descriptor. Belongs to loader.
                                           Not to be touched by workers. Only
                                           valid while IO is in-flight. */
    struct iovec *iovecs;               /* Array of MAX_SIZE / BLOCK_SIZE iovec
                                           structs used for liburing AIO. */
    size_t        n_vecs;               /* Number of structs in IOVECS. */
    uint8_t      *data;                 /* File data. */
    size_t        size;                 /* Size of file data in bytes. */
    size_t        max_size;             /* Maximum file data size in bytes. */

    /* Synchronization. */
    atomic_int flags;   /* ALLOCATED = 0b0001 -- Entry is currently being used.
                           IN FLIGHT = 0b0010 -- Entry has in-flight IO.
                           PENDING   = 0b0100 -- Actively being examined. Skip.
                           READY     = 0b1000 -- Data is ready, and this entry
                                                 has not yet been served to a
                                                 worker. */
} entry_t;

/* Worker state. Input/output queues unique to that worker. */
typedef struct {
    /* Input queue. */
    entry_t *queue;     /* CAPACITY queue entries. */
    size_t   next;      /* Next QUEUE entry to check. */
    size_t   used;      /* QUEUE entries currently in use. */
    size_t   capacity;  /* Total entries in QUEUE. */
} wstate_t;

/* Loader (reader + responder) state. */
typedef struct {
    wstate_t       *states;       /* N_STATES worker states. */
    size_t          n_states;     /* Worker states in STATES. */
    size_t          dispatch_n;   /* Async IO requests to send at once. */
    struct io_uring ring;         /* Submission ring buffer for liburing. */
} lstate_t;

#endif
