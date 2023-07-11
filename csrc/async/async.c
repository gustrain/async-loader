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

#include "../utils/utils.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

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

/* TODO.
   Loop for reader thread. */
void
async_reader_loop(void)
{
    /* Wait until length of output queue < max. */

    /* Loop through input queue. */

    /* Find entry with allocated = true, in_flight = false, atomically set
       in_flight to true (though atomics may not be needed, unless we have
       multiple loader processes/threads). */

    /* Ensure file exists. */

    return;
}

/* TODO.
   Loop for responder thread. */
void
async_responder_loop(void)
{
    return;
}

/* TODO.
   Given a loader, starts the reader and responder threads. Does not return. */
void
async_start(lstate_t *loader)
{
    return;
}

/* TODO.
   Initialize the loader. Allocates all shared memory. On success, initializes
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
    size_t worker_size = queue_size + sizeof(wstate_t);
    size_t total_size = worker_size * n_workers;

    /* Do the allocation. */
    if ((loader->states = mmap_alloc(total_size)) == NULL) {
        return -ENOMEM;
    }

    /*   LO                          HI
        ┌────────┬───────┬─────────────┐
        │wstate_t│entry_t│    file     │
        │structs │structs│    data     │
        └┬───────┴┬──────┴┬────────────┘
         │        │       │
         │        │       └►n_workers * max_file_size
         │        └►n_workers * queue_depth * sizeof(entry_t)
         └►n_workers * sizeof(wstate_t)
    */

    entry_t *entries_start = n_workers * sizeof(wstate_t);
    uint8_t *data_start = entries_start + n_workers * queue_depth * sizeof(entry_t);

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

            entry->data = data_start + (entry_n++) * max_file_size;
            entry->allocated = false;
            entry->in_flight = false;
            entry->ready = false;
            entry->max_size = max_file_size;
            entry->path[0] = '\0';
            entry->size = 0;
        }
    }

    /* Set the loader's config states. */
    loader->n_states = n_workers;
    loader->dispatch_n = min_dispatch_n;

    return 0;
}