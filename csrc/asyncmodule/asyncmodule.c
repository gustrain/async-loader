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

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "../async/async.h"
#include "../utils/alloc.h"

#define MAX_ERR_LEN (256)

/* Input validation. */
#define ARG_CHECK(valid_condition, error_string, return_fail)                  \
   if (!(valid_condition)) {                                                   \
      PyErr_SetString(PyExc_Exception, error_string);                          \
      return return_fail;                                                      \
   }

/* Python wrapper for lstate_t type. */
typedef struct {
   PyObject_HEAD

   lstate_t *loader;    /* Asynchronous loader state. */

} PyLoader;

/* PyLoader deallocate method. */
static void
PyLoader_dealloc(PyObject *self)
{
   PyLoader *loader = (PyLoader *) self;
   if (loader == NULL) {
      return;
   }

   if (loader->loader != NULL) {
      /* All of the lstate_t's shared memory is allocated contiguously in a
         single mmap, so freeing it is simple. */
      if (loader->loader->states != NULL) {
         mmap_free(loader->loader->states, loader->loader->total_size);
      }

      /* Free the lstate_t struct itself. */
      mmap_free(loader->loader, sizeof(lstate_t));
   }

   /* Free the PyLoader wrapper. */
   Py_TYPE(loader)->tp_free((PyObject *) loader);
}

/* PyLoader allocation method. */
static PyObject *
PyLoader_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
   /* Allocate the PyLoader struct. */
   PyLoader *self;
   if ((self = (PyLoader *) type->tp_alloc(type, 0)) == NULL) {
      PyErr_NoMemory();
      return NULL;
   }

   return (PyObject *) self;
}

/* PyLoader initialization method. */
static int
PyLoader_init(PyObject *self, PyObject *args, PyObject *kwds)
{
   PyLoader *loader = (PyLoader *) self;

   /* Parse arguments. */
   size_t queue_depth, max_file_size, n_workers, min_dispatch_n;
   static char *kwlist[] = {
      "queue_depth", "max_file_size", "n_workers", "min_dispatch_n"
   };
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "kkkk", kwlist,
                                    &queue_depth,
                                    &max_file_size,
                                    &n_workers,
                                    &min_dispatch_n)) {
      PyErr_SetString(PyExc_Exception, "missing/invalid argument");
      return -1;
   }

   /* Sanity-check arguments. */
   ARG_CHECK(queue_depth > 0, "queue depth must be positive", -1);
   ARG_CHECK(max_file_size > 0, "max file size must be positive", -1);
   ARG_CHECK(n_workers > 0, "must have >=1 worker(s)", -1);

   /* Allocate lstate using shared memory. */
   if ((loader->loader = mmap_alloc(sizeof(lstate_t))) == NULL) {
      PyErr_SetString(PyExc_Exception, "failed to allocate loader struct");
      return -1;
   }

   /* Initialize the loader. */
   int status = async_init(loader->loader,
                           queue_depth,
                           max_file_size,
                           n_workers,
                           min_dispatch_n);
   if (status < 0) {
      char error_string[MAX_ERR_LEN];
      snprintf(error_string,
               MAX_ERR_LEN,
               "failed to initialize loader; %s",
               strerror(-status));

      PyErr_SetString(PyExc_Exception, error_string);
      return -1;
   }

   return 0;
}
