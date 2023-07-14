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


   /* Sanity-check arguments. */


   /* Allocate lstate using shared memory. */


   /* Initialize the loader. */


   return 0;
}
