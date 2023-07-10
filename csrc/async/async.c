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

/* ------------- */
/*   INTERFACE   */
/* ------------- */

/* TODO. Worker interface to input queue. On success, inserts a request into the
   input queue and returns true. On failure, returns false. */
bool
async_request_file(wstate_t *state, char *path)
{
    return;
}

/* TODO. Worker interface to output queue. On success, returns a pointer to the
   beginning of a file in the output queue, and sets SIZE to the size of the
   file in bytes. On failure, NULL is returned and SIZE is unmodified. */
uint8_t *
async_try_get_file(wstate_t *state, size_t *size)
{
    return NULL;
}

/* TODO. Marks an entry in the output queue as complete (reclaimable). */
void
async_mark_done(wstate_t *state, uint8_t *data)
{
    return;
}


/* ----------- */
/*   BACKEND   */
/* ----------- */

/* TODO. Loop for reader thread. */
void
async_reader_loop(void)
{

}

/* TODO. Loop for responder thread. */
void
async_responder_loop(void)
{

}