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
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <liburing.h>


/* Insert ELEM into a doubly linked list, maintaining FIFO order. */
void
fifo_insert(entry_t **head, pthread_spinlock_t *lock, entry_t *elem)
{
    /* Handle case of empty list. */
    pthread_spin_lock(lock);
    if (*head == NULL) {
        *head = elem;
        elem->prev = NULL;
        elem->next = NULL;
        pthread_spin_unlock(lock);
        return;
    }

    /* Otherwise, insert into back of list. */
    elem->prev = (*head)->prev;
    elem->next = (*head);
    (*head)->prev->next = elem;
    (*head)->prev = elem;

    pthread_spin_unlock(lock);
}

/* Pop from a doubly linked list, maintaining FIFO order. */
entry_t *
fifo_pop(entry_t **head, pthread_spinlock_t *lock)
{
    pthread_spin_lock(lock);
    entry_t *out = *head;
    if (out == NULL) {
        pthread_spin_unlock(lock);
        return NULL;
    }

    (*head)->next->prev = (*head)->prev;
    (*head)->prev->next = (*head)->next;
    *head = (*head)->next;

    /* Handle case of resulting list being empty. */
    if (*head = out) {
        *head = NULL;
    }
    pthread_spin_unlock(lock);

    return out;
}

/* ------------- */
/*   INTERFACE   */
/* ------------- */

/* Worker interface to input queue. On success, inserts a request into the
   input queue and returns true. On failure, returns false (queue full). */
bool
async_try_request(wstate_t *state, char *path)
{
    for (size_t i = 0; i < state->capacity; i++);
    /* Claim an entry from the free list. */

    /* Configure entry. */

    /* Move entry to ready list. */

    return false;
}

/* Worker interface to output queue. On success, pops an entry from the
   completed queue and returns a pointer to it. On failure (e.g., list empty),
   NULL is returned and SIZE is unmodified. */
entry_t *
async_try_get(wstate_t *state)
{
    /* Try to get an entry from the completed list. Return NULL if empty. */
    return fifo_pop(&state->completed, &state->completed_lock);
}

/* Marks an entry in the output queue as complete (reclaimable). Pending flag
   must be held for the entry when calling this function. */
void
async_release(wstate_t *state, entry_t *e)
{
    /* Close the file used. */
    close(e->fd);

    /* Reset state. */
    e->size = 0;
    e->path[0] = '\0';
    e->fd = -1;

    /* Insert into the free list. */
    fifo_insert(&state->free, &state->free_lock, e);
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

/* Submits an AIO for the file at PATH, for a maximum of MAX_SIZE bytes to be
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
       visit to each worker's queue, if that queue has a valid request. */
    size_t i = 0;
    entry_t *e = NULL;
    while (true) {
        wstate_t *st = &ld->states[i++ % ld->n_states];

        /* Take an item from the ready list. */
        if ((e = fifo_pop(&st->ready, &st->ready_lock)) == NULL) {
            continue;
        }

        /* Issue the IO for this entry's filepath. */
        if (async_perform_io(ld, e) < 0) {
            /* What to do on failure? */
            fifo_insert(&st->ready, &st->ready_lock, e);
            continue;
        };
    }
}

/* Loop for responder thread. */
void *
async_responder_loop(lstate_t *ld)
{
    struct io_uring_cqe *cqe;
    while (true) {
        int status = io_uring_wait_cqe(&ld->ring, &cqe);
        if (status < 0) {
            printf(stderr, "io_uring_wait_cqe failed\n");
            continue;
        } else if (cqe->res < 0) {
            fprintf(stderr, "async read failed\n");
            continue;
        }

        /* Get the entry associated with the IO, and place it into the list for
           entries with completed IO. */
        entry_t *e = io_uring_cqe_get_data(cqe);
        fifo_insert(&e->worker->completed, &e->worker->completed_lock, e);
    }
    return;
}

/* Given a loader, starts the reader and responder threads. Does not return. */
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

        state->capacity = queue_depth;

        /* Assign memory for queues and file data. */
        state->queue = &entries_start[entry_n];
        for (size_t j = 0; j < queue_depth; j++) {
            entry_t *e = &state->queue[j];

            e->data = data_start + (entry_n) * max_file_size;
            e->n_vecs = max_file_size / BLOCK_SIZE;
            e->iovecs = iovec_start + entry_n * queue_depth * e->n_vecs * sizeof(struct iovec);
            for (size_t k = 0; k < e->n_vecs; k++) {
                /* Assign each iovec contiguously in the entry's data region. */
                e->iovecs[k].iov_base = e->data + k * BLOCK_SIZE;
                e->iovecs[k].iov_len = BLOCK_SIZE;

            }

            /* Configure entry. */
            e->max_size = max_file_size;
            e->path[0] = '\0';
            e->worker = state;
            e->size = 0;

            /* Link this entry to the following and previous entries, in order
               to initialize the free list with all entries. */
            e->next = &state->queue[(j + 1) % queue_depth];
            e->prev = &state->queue[(j - 1) % queue_depth];

            entry_n++;
        }

        /* Initialize status lists. */
        state->free = state->queue;
        state->ready = NULL;
        state->completed = NULL;
        state->served = NULL;

        /* Initialize the status list locks. */
        pthread_spinlock_init(&state->free_lock);
        pthread_spinlock_init(&state->ready_lock);
        pthread_spinlock_init(&state->completed_lock);
        pthread_spinlock_init(&state->served_lock);
    }

    /* Set the loader's config states. */
    loader->n_states = n_workers;
    loader->dispatch_n = min_dispatch_n;

    /* Initialize liburing. We don't need to worry about this not using shared
       memory because while worker interact with the shared queues, the IO
       submissions (thus interactions with liburing) are done only by this
       reader/responder process. */
    if (io_uring_queue_init(n_workers * queue_depth, &loader->ring, 0) != 0) {
        mmap_free(loader->states, total_size);
        return -errno;
    }

    return 0;
}