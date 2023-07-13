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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include "../../csrc/utils/alloc.h"
#include "../../csrc/async/async.h"


/* Generic worker process. */
void
test_worker_loop(wstate_t *worker,
                 uint64_t id,
                 char **filepaths,
                 size_t n_filepaths)
{
    struct timespec start, request_end, retrieve_end, release_end;
    entry_t *entries[n_filepaths];

    /* Request all files to loader. */
    clock_gettime(CLOCK_REALTIME, &start);
    for (size_t i = 0; i < n_filepaths; i++) {
        while (!async_try_request(worker, filepaths[i])) {}
    }
    clock_gettime(CLOCK_REALTIME, &request_end);

    printf("Submitted all requests.\n");


    /* Retrieve all files from loader. */
    for (size_t i = 0; i < n_filepaths; i++) {
        while ((entries[i] = async_try_get(worker)) == NULL) {}
    }
    clock_gettime(CLOCK_REALTIME, &retrieve_end);

    printf("Retrieved all entries.\n");

    /* Release all entries. */
    for (size_t i = 0; i < n_filepaths; i++) {
        async_release(entries[i]);
    }
    clock_gettime(CLOCK_REALTIME, &release_end);

    printf("Released all entries.\n");


    /* Log timing data. */
    long request_time = request_end.tv_nsec - start.tv_nsec + (request_end.tv_sec - start.tv_sec) * 1e9;
    long retrieve_time = retrieve_end.tv_nsec - start.tv_nsec + (retrieve_end.tv_sec - start.tv_sec) * 1e9;
    long release_time = release_end.tv_nsec - start.tv_nsec + (release_end.tv_sec - start.tv_sec) * 1e9;

    printf("Worker results --\n"
           "\tRequest time:  %ld ns\n"
           "\tRetrieve time: %ld ns (delta %ld ns)\n"
           "\tRelease time:  %ld ns (delta %ld ns)\n",
           request_time,
           retrieve_time, retrieve_time - request_time,
           release_time, release_time - retrieve_time);
}


void
test_config(size_t queue_depth,
            size_t max_file_size,
            size_t n_workers,
            size_t min_dispatch_n,
            char **filepaths,
            size_t n_filepaths)
{
    /* Create the loader. */
    lstate_t *loader = mmap_alloc(sizeof(lstate_t));
    assert(loader != NULL);

    /* Create a tracker for active worker processes. */
    atomic_size_t n_active_workers;
    atomic_store(&n_active_workers, n_workers);

    /* Initialize the loader. */
    int status = async_init(loader, queue_depth, max_file_size, n_workers, min_dispatch_n);
    assert(status == 0);

    /* Fork, spawning loader process and worker processes. */
    size_t fp_per_worker = n_filepaths / n_workers;
    for (size_t i = 0; i < n_workers; i++) {
        if (fork() > 0) {
            continue;
        }

        /* Start child as a worker. */
        test_worker_loop(&loader->states[i], i, filepaths + fp_per_worker * i, fp_per_worker);

        /* On worker termination, decrement number of active workers. If we were
           the last worker, kill the parent. */
        if (atomic_fetch_sub(&n_active_workers, 1) == 0) {
            printf("Final worker has terminated; killing loader.\n");
            kill(getppid(), 9);
        }

        /* Once finished, exit. */
        exit(EXIT_SUCCESS);
    }

    /* Parent becomes the loader once all workers have been created. */
    async_start(loader);
}

int
main(int argc, char **argv)
{
    size_t queue_depth    = 32;
    size_t max_file_size  = 1024 * 1024;
    size_t n_workers      = 1;
    size_t min_dispatch_n = queue_depth;
    size_t n_filepaths    = 4;
    char *filepaths[] = {
        "Makefile",
        "test",
        "test_async.c",
        "test_async.o",
    };

    test_config(queue_depth,
                max_file_size,
                n_workers,
                min_dispatch_n,
                filepaths,
                n_filepaths);

    return EXIT_SUCCESS;
}