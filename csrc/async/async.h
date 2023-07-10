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

#define MAX_PATH_LEN (128)

/* Input queue entry. */
typedef struct {
    char        path[MAX_PATH_LEN];     /* Filepath of the requested file. */

    /* Synchronization. */
    atomic_bool allocated;  /* Entry is currently being used. */
    atomic_bool in_flight;  /* Entry has in-flight IO. */
} iq_entry_t;

/* Output queue entry. */
typedef struct {
    uint8_t *data;      /* File data. */
    size_t   size;      /* Size of file data in bytes. */
    size_t   max_size;  /* Maximum size of file data in bytes. */

    /* Synchronization. */
    atomic_bool allocated;  /* Entry is currently being used. */
    atomic_bool ready;      /* Data is ready, and this entry has not yet been
                               served to a worker. */
} oq_entry_t;

/* Worker state. Input/output queues unique to that worker. */
typedef struct {
    /* Input queue. */
    iq_entry_t *in_queue;       /* IN_CAPACITY input queue entries. */
    iq_entry_t *in_next;        /* Next IN_QUEUE entry to check. */
    size_t      in_used;        /* IN_QUEUE entries currently in use. */
    size_t      in_capacity;    /* Total entries in IN_QUEUE. */

    /* Output queue. */
    oq_entry_t *out_queue;      /* OUT_CAPACITY output queue entries. */
    oq_entry_t *out_next;       /* Next OUT_QUEUE entry to check. */
    size_t      out_used;       /* OUT_QUEUE entries currently in used. */
    size_t      out_capacity;   /* Total entries in OUT_QUEUE. */
} wstate_t;

/* Loader (reader + responder) state. */
typedef struct {
    wstate_t *states;       /* N_STATES worker states. */
    size_t    n_states;     /* Number of worker states in STATES. */

    size_t    dispatch_n;   /* Number of async IO requests to send at once. */
} lstate_t;

#endif
