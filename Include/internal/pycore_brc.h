#ifndef Py_INTERNAL_BRC_H
#define Py_INTERNAL_BRC_H

#include <stdint.h>
#include "pycore_llist.h"           // struct llist_node
#include "pycore_object_stack.h"    // _PyObjectStack

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#ifdef Py_GIL_DISABLED

// Prime number to avoid correlations with memory addresses.
#define _Py_BRC_NUM_BUCKETS 257

// Hash table bucket
struct _brc_bucket {
    // Mutex protects both the bucket and thread state queues in this bucket.
    PyMutex mutex;

    // Linked list of _PyThreadStateImpl objects hashed to this bucket.
    struct llist_node root;
};

// Per-interpreter biased reference counting state
struct _brc_state {
    // Hash table of thread states by thread-id. Thread states within a bucket
    // are chained using a doubly-linked list.
    struct _brc_bucket table[_Py_BRC_NUM_BUCKETS];
};

// Per-thread biased reference counting state
struct _brc_thread_state {
    // Linked-list of thread states per hash bucket
    struct llist_node bucket_node;

    // Thread-id as determined by _PyThread_Id()
    uintptr_t tid;

    // Objects with refcounts to be merged (protected by bucket mutex)
    _PyObjectStack objects_to_merge;

    // Local stack of objects to be merged (not accessed by other threads)
    _PyObjectStack local_objects_to_merge;

    // While `defer_depth` > 0, objects this thread queues for other threads
    // are collected in `deferred` and handed over by _Py_brc_flush_deferred()
    // (not accessed by other threads).
    int defer_depth;
    _PyObjectStack deferred;
};

// Initialize/finalize the per-thread biased reference counting state
void _Py_brc_init_thread(PyThreadState *tstate);
void _Py_brc_remove_thread(PyThreadState *tstate);

// Initialize per-interpreter state
void _Py_brc_init_state(PyInterpreterState *interp);

void _Py_brc_after_fork(PyInterpreterState *interp);

// Enqueues an object to be merged by it's owning thread (tid). This
// steals a reference to the object.
void _Py_brc_queue_object(PyObject *ob);

// Merge the refcounts of queued objects for the current thread.
void _Py_brc_merge_refcounts(PyThreadState *tstate);

// Batch the objects the current thread queues for other threads, e.g. while
// the GC frees garbage owned by other threads, so that each owner's bucket
// lock is taken once per batch instead of once per object.
void _Py_brc_begin_deferred(PyThreadState *tstate);
void _Py_brc_end_deferred(PyThreadState *tstate);

#endif /* Py_GIL_DISABLED */

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_BRC_H */
