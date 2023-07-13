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
#include <unistd.h>
#include "../../csrc/utils/alloc.h"
#include "../../csrc/async/async.h"


/* Generic worker process. */
int
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
        while (!async_try_request(worker, filepaths[i])) {
            printf("read submission failed; retrying\n");
        }
    }
    clock_gettime(CLOCK_REALTIME, &request_end);


    /* Retrieve all files from loader. */
    for (size_t i = 0; i < n_filepaths; i++) {
        while ((entries[i] = async_try_get(worker)) == NULL) {
            printf("retrieve failed; retrying\n");
        }
    }
    clock_gettime(CLOCK_REALTIME, &retrieve_end);

    /* Release all entries. */
    for (size_t i = 0; i < n_filepaths; i++) {
        async_release(entries[i]);
    }
    clock_gettime(CLOCK_REALTIME, &release_end);


    /* Log timing data. */
    long request_time = ((start.tv_nsec + start.tv_sec * 1e9) - (request_end.tv_nsec + request_end.tv_sec * 1e9)) / (1000 * 1000);
    long retrieve_time = ((start.tv_nsec + start.tv_sec * 1e9) - (retrieve_end.tv_nsec + retrieve_end.tv_sec * 1e9)) / (1000 * 1000);
    long release_time = ((start.tv_nsec + start.tv_sec * 1e9) - (release_end.tv_nsec + release_end.tv_sec * 1e9)) / (1000 * 1000);

    printf("Worker results --\n"
           "\tRequest time:  %.4lu ms\n"
           "\tRetrieve time: %.4lu ms (delta %.4lu ms)\n"
           "\tRelease time:  %.4lu ms (delta %.4lu ms)\n",
           request_time,
           retrieve_time, retrieve_time - request_time,
           release_time, release_time - retrieve_time);
    
    return 0;
}


int
test_config(size_t queue_depth,
            size_t max_file_size,
            size_t n_workers,
            size_t min_dispatch_n,
            char **filepaths,
            size_t n_filepaths)
{
    /* Create the loader. */
    lstate_t *loader = mmap_alloc(sizeof(lstate_t));
    if (loader == NULL) {
        return -ENOMEM;
    }

    /* Initialize the loader. */
    int status =  async_init(loader, queue_depth, max_file_size, n_workers, min_dispatch_n);
    if (status != 0) {
        return status;
    }


    /* Fork, spawning loader process and worker processes. */
    size_t fp_per_worker = n_filepaths / n_workers;
    if (fork() == 0) {
        /* Child. Spawn N_WORKERS - 1 additional workers, since one exists. */
        for (size_t i = 0; i < n_workers - 1; i++) {
            if (fork() > 0) {
                /* If we're the parent, loop and spawn another child. */
                continue;
            }

            /* Start child as a worker. */
            return test_worker_loop(&loader->states[i], i, filepaths + fp_per_worker * i, fp_per_worker);
        }

        /* Start parent as a worker. */
        return test_worker_loop(&loader->states[n_workers - 1], n_workers - 1, filepaths, n_filepaths);
    } else {
        /* Parent. Start parent as loader.*/
        async_start(loader);
        return 0;
    }
}

int
main(int argc, char **argv)
{
    size_t queue_depth    = 32;
    size_t max_file_size  = 1024 * 1024;
    size_t n_workers      = 4;
    size_t min_dispatch_n = queue_depth;
    size_t n_filepaths    = 2;
    char *filepaths[] = {
        "",
        "",
    };

    test_config(queue_depth,
                max_file_size,
                n_workers,
                min_dispatch_n,
                filepaths,
                n_filepaths);

    return EXIT_SUCCESS;
}