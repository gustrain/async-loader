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
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <liburing.h>
#include <string.h>
#include <time.h>
#include <linux/fs.h>
#include <linux/fiemap.h>


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

    return true;
}

/* Worker interface to output queue. On success, pops an entry from the
   completed queue and returns a pointer to it. On failure (e.g., list empty),
   NULL is returned. */
entry_t *
async_try_get(wstate_t *state)
{
    entry_t *e;

    /* Try to get an entry from the completed list. Return NULL if empty. This
       read is racy, but the only goal is to prevent hogging the lock when the
       list is empty. */
    if (state->completed != NULL) {
        e = fifo_pop(&state->completed, &state->completed_lock);
        /* Acquire shm object and mmap it so data may be accessed. */
        e->shm_wfd = shm_open(e->shm_fp, O_RDWR, S_IRUSR | S_IWUSR);
        assert(e->shm_wfd >= 0);
        e->shm_wdata = mmap(NULL, e->size, PROT_WRITE, MAP_SHARED, e->shm_wfd, 0);
        assert(e->shm_wdata != NULL);

        return e;
    }
    
    return NULL;
}

/* Marks an entry in the output queue as complete (reclaimable). Pending flag
   must be held for the entry when calling this function. */
void
async_release(entry_t *e)
{
    /* Unlink the shm object, and unmap the worker-side mmap. */
    shm_unlink(e->shm_fp);
    close(e->shm_wfd);
    munmap(e->shm_wdata, e->size);

    /* Insert into the free list. */
    fifo_push(&e->worker->free, &e->worker->free_lock, e);
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

/* Submits an AIO for the file at PATH, allocating an shm object of equal size
   to the file for the data to be read into. Saves the file descriptor to FD.
   On success, returns 0. On failure, returns negative ERRNO value. */
static int
async_perform_io(lstate_t *ld, entry_t *e)
{
    /* Unmap any previous mmap. */
    if (e->shm_lmapped) {
        munmap(e->shm_ldata, e->size);
        close(e->shm_lfd);
        e->shm_lmapped = false;
    }

    /* Path must not be empty. */
    assert(e->path[0] != '\0');

    /* Open the file, get and check size. */
    if ((e->fd = open(e->path, O_RDONLY)) < 0) {
        fprintf(stderr, "failed to open %s\n", e->path);
        return -errno;
    };

    /* Get the file's size. */
    off_t size = file_get_size(e->fd);
    if (size < 0) {
        close(e->fd);
        return (int) size;
    }
    e->size = (((size_t) size) | 0xFFF) + 1;

    /* Get fiemap with first extent. */
    uint8_t stack_mem[sizeof(struct fiemap) + sizeof(struct fiemap_extent)];
    struct fiemap *fiemap = (struct fiemap *) stack_mem;
    memset(fiemap, 0, sizeof(struct fiemap));
    fiemap->fm_length = ~0;
    fiemap->fm_extent_count = 1;
    if (ioctl(e->fd, FS_IOC_FIEMAP, fiemap) < 0) {
        fprintf(stderr, "ioctl failed (%s); %s\n", e->path, strerror(errno));
    } else {
        e->lba = fiemap->fm_extents[0].fe_physical;
    }

    /* Prepare the filepath according to shm requirements. */
    e->shm_fp[0] = '/';
    for (int i = 0; i < MAX_PATH_LEN + 1; i++) {
        /* Replace all occurences of '/' with '_'. */
        e->shm_fp[i + 1] = e->path[i] == '/' ? '_' : e->path[i];
        if (e->path[i] == '\0') {
            break;
        }
    }

    /* Allocate shm object. It will be the responsibility of the worker to call
       ASYNC_RELEASE in order to unlink this shm object. */
    e->shm_lfd = shm_open(e->shm_fp, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (e->shm_lfd < 0) {
        fprintf(stderr, "failed to shm_open %s\n", e->shm_fp);
        close(e->fd);
        return -errno;
    }

    /* Appropriately size the shm object. */
    if (ftruncate(e->shm_lfd, e->size) < 0) {
        shm_unlink(e->shm_fp);
        close(e->shm_lfd);
        close(e->fd);
        return -errno;
    }

    /* Create mmap for the shm object. */
    e->shm_ldata = mmap(NULL, e->size, PROT_WRITE, MAP_SHARED, e->shm_lfd, 0);
    if (e->shm_ldata == NULL) {
        shm_unlink(e->shm_fp);
        close(e->shm_lfd);
        close(e->fd);
        return -ENOMEM;
    }
    e->shm_lmapped = true;

    /* Create and submit the uring AIO request. */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ld->ring);
    io_uring_prep_read(sqe, e->fd, e->shm_ldata, e->size, 0);
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

        /* Pop an item from the ready list. Racy check to avoid hogging lock. */
        if ((e = fifo_pop(&st->ready, &st->ready_lock)) == NULL) {
            continue;
        }

        /* Issue the IO for this entry's filepath. */
        int status = async_perform_io(ld, e);
        if (status < 0) {
            /* What to do on failure? */
            fprintf(stderr,
                    "reader failed to issue IO; %s; %s; %s.\n",
                    e->path,
                    e->shm_fp,
                    strerror(-status));
            fifo_push(&st->ready, &st->ready_lock, e);
        }
    }

    return NULL;
}

/* Loop for responder thread. */
static void *
async_responder_loop(void *arg)
{
    lstate_t *ld = (lstate_t *) arg;

    int cnt = 0;

    struct io_uring_cqe *cqe;
    while (true) {
        /* Remove an entry from the completion queue. */
        int status = io_uring_wait_cqe(&ld->ring, &cqe);
        if (status < 0) {
            fprintf(stderr,
                    "io_uring_wait_cqe failed; %s.\n",
                    strerror(-status));
            continue;
        } else if (cqe->res < 0) {
            entry_t *e = io_uring_cqe_get_data(cqe);
            fprintf(stderr,
                    "asynchronous read failed; %s (data @ %p, size = 0x%lx).\n",
                    strerror(-cqe->res),
                    e->shm_ldata,
                    e->size);
            if (cnt++ > 32) {
                exit(EXIT_FAILURE);
            }
            continue;
        }

        /* Get the entry associated with the IO, and place it into the list for
           entries with completed IO. */
        entry_t *e = io_uring_cqe_get_data(cqe);
        io_uring_cqe_seen(&ld->ring, cqe);
        close(e->fd);
        fifo_push(&e->worker->completed, &e->worker->completed_lock, e);
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
        fprintf(stderr,
                "failed to created reader thread; %s\n",
                strerror(-status));
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
    /* Figure out how much memory to allocate. */
    size_t entry_size = sizeof(entry_t);
    size_t queue_size = entry_size * queue_depth;
    size_t worker_size = queue_size + sizeof(wstate_t);
    size_t total_size = worker_size * n_workers;

    /* Do the allocation. */
    if ((loader->states = mmap_alloc(total_size)) == NULL) {
        return -ENOMEM;
    }

    /*   LO            HI
        ┌────────┬───────┐
        │wstate_t│entry_t│
        │structs │structs│
        └┬───────┴┬──────┘
         │        │
         │        │
         │        │
         │        └►n_workers * queue_depth * sizeof(entry_t)
         └►n_workers * sizeof(wstate_t)
    */

    /* Sizes of each region. */
    size_t state_bytes = n_workers * sizeof(wstate_t);

    /* Addresses of each region. */
    entry_t *entry_start = (entry_t *) ((uint8_t *) loader->states + state_bytes);

    /* Assign all of the correct locations to each state/queue. */
    size_t entry_n = 0;
    for (size_t i = 0; i < n_workers; i++) {
        wstate_t *state = &loader->states[i];

        state->capacity = queue_depth;

        /* Assign memory for queues and file data. */
        state->queue = &entry_start[entry_n];
        for (size_t j = 0; j < queue_depth; j++) {
            entry_t *e = &state->queue[j];

            /* Configure SHM. */
            e->shm_fp[0] = 0;
            e->shm_lfd = -1;
            e->shm_wfd = -1;
            e->shm_ldata = NULL;
            e->shm_wdata = NULL;
            e->shm_lmapped = false;

            /* Configure entry. */
            e->path[0] = '\0';
            e->worker = state;
            e->size = 0;
            e->fd = -1;

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
    int status = io_uring_queue_init((unsigned int) (n_workers * queue_depth),
                                     &loader->ring,
                                     0);
    if (status < 0) {
        fprintf(stderr, "io_uring_queue_init failed; %s\n", strerror(-status));
        mmap_free(loader->states, total_size);
        return status;
    }

    return 0;
}