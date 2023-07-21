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
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <liburing.h>
#include <string.h>
#include <time.h>

#define LOG_STATE_CHANGE(label, entry)                                                                                 \
    char buf[512];                                                                                                     \
    get_time(&buf, 512);                                                                                               \
    printf("%22s | %90s | %16p | %s\n", "FREE -> READY", entry->path, entry, buf)

/* Debug, get time. */
get_time(char buf[512], size_t size) {
    time_t rawtime;
    struct tm * timeinfo;

    time(&rawtime);
    timeinfo = localtime (&rawtime);
    snprintf(buf, size, "Current local time and date: %s", asctime(timeinfo));
}

/* Insert ELEM into a doubly linked list, maintaining FIFO order. */
static void
fifo_push(entry_t **head, pthread_spinlock_t *lock, entry_t *elem)
{
    /* Handle case of empty list. */
    pthread_spin_lock(lock);
    if (*head == NULL) {
        *head = elem;
        elem->prev = elem;
        elem->next = elem;
        pthread_spin_unlock(lock);
        return;
    }

    /* Otherwise, insert into back of list. */
    elem->prev = (*head)->prev;
    elem->next = (*head)->next;

    /* Place this element behind the current tail. */
    (*head)->prev->next = elem;
    (*head)->prev = elem;

    pthread_spin_unlock(lock);
}

/* Pop from a doubly linked list, maintaining FIFO order. */
static entry_t *
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

    /* Handle case of resulting list being empty. */
    *head = (*head)->next;
    if (*head == out) {
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
    /* Get a free entry. Return false if none available. */
    entry_t *e = fifo_pop(&state->free, &state->free_lock);
    if (e == NULL) {
        fprintf(stderr, "free list is empty.\n");
        return false;
    }

    /* Configure the entry and move it into the ready list. */
    strncpy(e->path, path, MAX_PATH_LEN);
    fifo_push(&state->ready, &state->ready_lock, e);

    LOG_STATE_CHANGE("FREE -> READY", e);

    return true;
}

/* Worker interface to output queue. On success, pops an entry from the
   completed queue and returns a pointer to it. On failure (e.g., list empty),
   NULL is returned. */
entry_t *
async_try_get(wstate_t *state)
{
    /* Try to get an entry from the completed list. Return NULL if empty. This
       read is racy, but the only goal is to prevent hogging the lock when the
       list is empty. */
    if (state->completed != NULL) {
        entry_t *e = fifo_pop(&state->completed, &state->completed_lock);
        LOG_STATE_CHANGE("COMPLETED -> SERVED", e);
        return e;
    }
    
    return NULL;
}

/* Marks an entry in the output queue as complete (reclaimable). Pending flag
   must be held for the entry when calling this function. */
void
async_release(entry_t *e)
{
    /* Insert into the free list. */
    fifo_push(&e->worker->free, &e->worker->free_lock, e);

    LOG_STATE_CHANGE("SERVED -> FREE", e);
}


/* ----------- */
/*   BACKEND   */
/* ----------- */

/* On success, returns the size of a file in bytes. On failure, returns negative
   ERRNO value. */
static off_t
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
static int
async_perform_io(lstate_t *ld, entry_t *e)
{
    /* Open the file, get and check size. */
    if ((e->fd = open(e->path, O_RDONLY)) < 0) {
        return -errno;
    };

    /* Get the file's size, check it's within bounds. */
    off_t size = file_get_size(e->fd);
    if (size < 0 || (size_t) size > e->max_size) {
        close(e->fd);
        return -E2BIG;
    }
    e->size = (size_t) size;

    /* Create and submit the uring AIO request. */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ld->ring);
    io_uring_prep_readv(sqe, e->fd, e->iovecs, e->n_vecs, 0);
    io_uring_sqe_set_data(sqe, e);  /* Associate request with this entry. */
    io_uring_submit(&ld->ring);

    return 0;
}

/* Loop for reader thread. */
static void *
async_reader_loop(void *arg)
{
    lstate_t *ld = (lstate_t *) arg;

    /* Loop through the outer states array round-robin style, issuing one IO per
       visit to each worker's queue, if that queue has a valid request. */
    size_t i = 0;
    entry_t *e = NULL;
    while (true) {
        wstate_t *st = &ld->states[i++ % ld->n_states];

        /* Take an item from the ready list. Racy check to avoid hogging lock. */
        if ((e = fifo_pop(&st->ready, &st->ready_lock)) == NULL) {
            continue;
        }

        /* Issue the IO for this entry's filepath. */
        int status = async_perform_io(ld, e);
        if (status < 0) {
            /* What to do on failure? */
            fprintf(stderr, "reader failed to issue IO; %s; %s.\n", e->path, strerror(-status));
            fifo_push(&st->ready, &st->ready_lock, e);
            LOG_STATE_CHANGE("READY -> READY", e);
            continue;
        };
        LOG_STATE_CHANGE("READY -> IO_URING", e);
    }

    return NULL;
}

/* Loop for responder thread. */
static void *
async_responder_loop(void *arg)
{
    lstate_t *ld = (lstate_t *) arg;

    struct io_uring_cqe *cqe;
    while (true) {
        /* Remove an entry from the completion queue. */
        int status = io_uring_wait_cqe(&ld->ring, &cqe);
        if (status < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed; %s.\n", strerror(-status));
            continue;
        } else if (cqe->res < 0) {
            fprintf(stderr, "asynchronous read failed; %s.\n", strerror(-cqe->res));
            continue;
        }

        /* Get the entry associated with the IO, and place it into the list for
           entries with completed IO. */
        entry_t *e = io_uring_cqe_get_data(cqe);
        io_uring_cqe_seen(&ld->ring, cqe);
        close(e->fd);
        fifo_push(&e->worker->completed, &e->worker->completed_lock, e);
        LOG_STATE_CHANGE("IO_URING -> COMPLETED", e);
    }

    return NULL;
}

/* Given a loader, starts the reader and responder threads. Does not return. */
void
async_start(lstate_t *loader)
{
    /* Spawn the reader. */
    pthread_t reader;
    int status = pthread_create(&reader, NULL, async_reader_loop, loader);
    if (status < 0) {
        fprintf(stderr, "failed to created reader thread; %s\n", strerror(-status));
        assert(false);
    }

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
    /* Figure out how much memory to allocate. Allocate an extra BLOCK_SIZE
       bytes so that we have sufficient memory even after data alignment. */
    size_t entry_size = sizeof(entry_t) + max_file_size;
    size_t queue_size = entry_size * queue_depth;
    size_t worker_size = sizeof(struct iovec) + queue_size + sizeof(wstate_t);
    size_t total_size = worker_size * n_workers + BLOCK_SIZE;

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
         │        │       └►n_workers * queue_depth * sizeof(struct iovec)
         │        └►n_workers * queue_depth * sizeof(entry_t)
         └►n_workers * sizeof(wstate_t)
    */

    /* Sizes of each region. */
    size_t n_queue_entries = n_workers * queue_depth;
    size_t state_bytes = n_workers * sizeof(wstate_t);
    size_t entry_bytes = n_queue_entries * sizeof(entry_t);
    size_t iovec_bytes = n_queue_entries * sizeof(struct iovec);

    /* Addresses of each region. */
    uint8_t *entry_start = (uint8_t *) loader->states + state_bytes;
    uint8_t *iovec_start = entry_start + entry_bytes;
    uint8_t *data_start  = iovec_start + iovec_bytes;

    /* Ensure that data is block-aligned. */
    data_start += BLOCK_SIZE - (((uint64_t) data_start) % BLOCK_SIZE);
    assert(((uint64_t) data_start) % BLOCK_SIZE == 0);

    /* Assign all of the correct locations to each state/queue. */
    size_t entry_n = 0;
    for (size_t i = 0; i < n_workers; i++) {
        wstate_t *state = &loader->states[i];

        state->capacity = queue_depth;

        /* Assign memory for queues and file data. */
        state->queue = (entry_t *) (entry_start + entry_n * sizeof(entry_t));
        for (size_t j = 0; j < queue_depth; j++) {
            entry_t *e = &state->queue[j];

            /* Data needs to be block-aligned. */
            e->data = data_start + entry_n * max_file_size;
            assert(((uint64_t) e->data) % BLOCK_SIZE == 0);

            /* Configure the iovec for asynchronous readv. */
            e->n_vecs = 1;
            e->iovecs = (struct iovec *) (iovec_start + entry_n * queue_depth *
                                          e->n_vecs * sizeof(struct iovec));
            e->iovecs[1].iov_base = e->data;
            e->iovecs[1].iov_len = max_file_size;

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

        /* Initialize the status list locks. */
        pthread_spin_init(&state->free_lock, PTHREAD_PROCESS_SHARED);
        pthread_spin_init(&state->ready_lock, PTHREAD_PROCESS_SHARED);
        pthread_spin_init(&state->completed_lock, PTHREAD_PROCESS_SHARED);
    }

    /* Set the loader's config states. */
    loader->n_states = n_workers;
    loader->dispatch_n = min_dispatch_n;
    loader->total_size = total_size;

    /* Initialize liburing. We don't need to worry about this not using shared
       memory because while worker interact with the shared queues, the IO
       submissions (thus interactions with liburing) are done only by this
       reader/responder process. */
    int status = io_uring_queue_init(n_workers * queue_depth, &loader->ring, 0);
    if (status < 0) {
        fprintf(stderr, "io_uring_queue_init failed; %s\n", strerror(-status));
        mmap_free(loader->states, total_size);
        return status;
    }

    return 0;
}