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

#include "async.h"

#include "../utils/alloc.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <liburing.h>

/* ------------- */
/*   INTERFACE   */
/* ------------- */

/* TODO.
   Worker interface to input queue. On success, inserts a request into the
   input queue and returns true. On failure, returns false (queue full). */
bool
async_request(wstate_t *state, char *path)
{
    /* Loop to find free entry. */

    /* Atomically claim entry (allocated => true). */

    /* Configure entry. */

    /* Atomically mark entry ready. */

    return false;
}

/* TODO.
   Worker interface to output queue. On success, returns a pointer to the
   beginning of a file in the output queue, and sets SIZE to the size of the
   file in bytes. On failure, NULL is returned and SIZE is unmodified. */
uint8_t *
async_try_get(wstate_t *state, size_t *size)
{
    /* Loop through the output queue to find an entry. */

    /* Atomically claim entry. */

    /* Return entry->data. */

    return NULL;
}

/* TODO.
   Marks an entry in the output queue as complete (reclaimable). */
void
async_release(wstate_t *state, uint8_t *data)
{
    /* Atomically mark unallocated. */

    /* Atomically decreased used quantity. */

    /* I think this order is OK? */

    return;
}


/* ----------- */
/*   BACKEND   */
/* ----------- */

/* On success, returns the size of a file in bytes. On failure, returns negative
   ERRNO value. */
off_t
file_get_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) {
        return -errno;
    }

    /* Check device type. */
    if (S_ISBLK(st.st_mode)) {
        /* Block device. */
        uint64_t bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            return -errno;
        }
        
        return bytes;
    } else if (S_ISREG(st.st_mode)) {
        return st.st_size;
    }
    
    /* Unknown device type. */
    return -1;
}

/* TODO.
   Submits an AIO for the file at PATH, for a maximum of MAX_SIZE bytes to be
   read into the buffer DATA. Saves the file descriptor to FD, and allows AIO to
   save number of bytes read to SIZE. On success, returns 0. On failure, returns
   negative ERRNO value. */
int
async_perform_io(lstate_t *ld, entry_t *e)
{
    /* Open the file, get and check size. */
    if ((e->fd = open(e->path, O_RDONLY)) < 0) {
        return -errno;
    };

    /* Get the file's size, check it's within bounds. */
    off_t size = file_get_size(e->fd);
    if (size < 0 || (size_t) size > e->max_size) {
        return -1;
    }
    e->size = (size_t) size;

    /* Create and submit the uring AIO request. */
    struct io_uring_seq *sqe = io_uring_get_seq(ld->ring);
    io_uring_prep_readv(sqe, e->fd, e->iovecs, e->n_vecs, 0);
    io_uring_sqe_set_data(sqe, e);  /* Associate request with this entry. */
    io_uring_submit(ld->ring);

    return 0;
}

/* Loop for reader thread. */
void *
async_reader_loop(lstate_t *ld)
{
    /* Loop through the outer states array round-robin style, issuing one IO per
       visit to each worker's queue. */
    size_t i = 0;
    while (true) {
        wstate_t *st = &ld->states[i++ % ld->n_states];

        /* Check the queue. */
        for (size_t j = 0; j < st->capacity; j++) {
            entry_t *e = &st->queue[(st->next + j) % st->capacity];

            /* Get the flags. Skip if already pending. */
            int flags = atomic_fetch_or(&e->flags, PENDING_FLAG);
            if (flags & PENDING_FLAG) {
                continue;
            }

            /* Otherwise, examine to see if it's a viable entry. */
            if (!(flags & ALLOCATED_FLAG) || (flags & IN_FLIGHT_FLAG)) {
                /* Toggle off the pending flag and try another entry. */
                atomic_store(&e->flags, flags ^ PENDING_FLAG);
                continue;
            }

            /* Issue the IO for this entry's filepath. */
            if (async_perform_io(ld, e) < 0) {
                /* What to do on failure? */
                atomic_store(&e->flags, flags ^ PENDING_FLAG);
                continue;
            };


            /* Toggle the pending flag off, and the in-flight flag on, once
                we've finished initiating the IO for this entry. */
            atomic_store(&e->flags, flags ^ (PENDING_FLAG | IN_FLIGHT_FLAG));
            break;
        }
    }
}

/* TODO.
   Loop for responder thread. */
void *
async_responder_loop(lstate_t *loader)
{
    return;
}

/* TODO.
   Given a loader, starts the reader and responder threads. Does not return. */
void
async_start(lstate_t *loader)
{
    pthread_t reader, responder;

    /* Spawn the reader. */
    int status = pthread_create(&reader, NULL, async_reader_loop, loader);
    assert(status = 0);

    /* Become the responder. */
    async_responder_loop(loader);

    /* Never reached. */
    assert(false);
}

/* Initialize the loader. Allocates all shared memory. On success, initializes
   LOADER and returns 0. On failure, returns negative ERRNO value. Each worker
   is given of queue of depth QUEUE_DEPTH, with each entry containing
   MAX_FILE_SIZE bytes for file data. IO is only dispatched when a minimum of
   MIN_DISPATCH_N IOs are ready to execute. */
int
async_init(lstate_t *loader,
           size_t queue_depth,
           size_t max_file_size,
           size_t n_workers,
           size_t min_dispatch_n)
{
    /* Figure out how much memory to allocate. */
    size_t entry_size = sizeof(entry_t) + max_file_size;
    size_t queue_size = entry_size * queue_depth;
    size_t iovec_size = sizeof(struct iovec) * max_file_size / BLOCK_SIZE;
    size_t worker_size = iovec_size + queue_size + sizeof(wstate_t);
    size_t total_size = worker_size * n_workers;

    /* Do the allocation. */
    if ((loader->states = mmap_alloc(total_size)) == NULL) {
        return -ENOMEM;
    }

    /*   LO                                  HI
        ┌────────┬───────┬───────┬─────────────┐
        │wstate_t│entry_t│iovec  │    file     │
        │structs │structs│structs│    data     │
        └┬───────┴┬──────┴┬──────┴┬────────────┘
         │        │       │       │
         │        │       │       └►n_workers * queue_depth * max_file_size
         │        │       └►n_workers * queue_depth * (max_file_size / BLOCK_SIZE) * sizeof(struct iovec)
         │        └►n_workers * queue_depth * sizeof(entry_t)
         └►n_workers * sizeof(wstate_t)
    */

    entry_t *entries_start = n_workers * sizeof(wstate_t);
    struct iovec *iovec_start = entries_start + n_workers * queue_depth * sizeof(entry_t);
    uint8_t *data_start = iovec_start + n_workers * queue_depth * (max_file_size / BLOCK_SIZE) * sizeof(struct iovec);

    /* Assign all of the correct locations to each state/queue. */
    size_t entry_n = 0;
    void *cur_pos = loader->states;
    for (size_t i = 0; i < n_workers; i++) {
        wstate_t *state = &loader->states[i];

        state->next = state;
        state->capacity = queue_depth;
        state->used = 0;

        /* Assign memory for queues and file data. */
        state->queue = &entries_start[entry_n];
        for (size_t j = 0; j < queue_depth; j++) {
            entry_t *entry = &state->queue[j];

            entry->data = data_start + (entry_n) * max_file_size;
            entry->n_vecs = max_file_size / BLOCK_SIZE;
            entry->iovecs = iovec_start + entry_n * queue_depth * entry->n_vecs * sizeof(struct iovec);
            for (size_t k = 0; k < entry->n_vecs; k++) {
                /* Assign each iovec contiguously in the entry's data region. */
                entry->iovecs[k].iov_base = entry->data + k * BLOCK_SIZE;
                entry->iovecs[k].iov_len = BLOCK_SIZE;
            }

            /* Configure entry. */
            atomic_store(&entry->flags, NONE_FLAG);
            entry->max_size = max_file_size;
            entry->path[0] = '\0';
            entry->size = 0;

            entry_n++;
        }
    }

    /* Set the loader's config states. */
    loader->n_states = n_workers;
    loader->dispatch_n = min_dispatch_n;

    /* Initialize liburing. */
    if (io_uring_queue_init(n_workers * queue_depth, &loader->ring, 0) != 0) {
        mmap_free(loader->states, total_size);
        return -errno;
    }

    return 0;
}