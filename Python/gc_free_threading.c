// Cyclic garbage collector implementation for free-threaded build.
#include "Python.h"
#include "pycore_brc.h"           // struct _brc_thread_state
#include "pycore_ceval.h"         // _Py_set_eval_breaker_bit()
#include "pycore_dict.h"          // _PyInlineValuesSize()
#include "pycore_frame.h"         // FRAME_CLEARED
#include "pycore_freelist.h"      // _PyObject_ClearFreeLists()
#include "pycore_genobject.h"     // _PyGen_GetGeneratorFromFrame()
#include "pycore_initconfig.h"    // _PyStatus_NO_MEMORY()
#include "pycore_interp.h"        // PyInterpreterState.gc
#include "pycore_interpframe.h"   // _PyFrame_GetLocalsArray()
#include "pycore_list.h"          // _PyList_GetItemRef()
#include "pycore_object_alloc.h"  // _PyObject_MallocWithType()
#include "pycore_pystate.h"       // _PyThreadState_GET()
#include "pycore_tstate.h"        // _PyThreadStateImpl
#include "pycore_tuple.h"         // _PyTuple_MaybeUntrack()
#include "pycore_weakref.h"       // _PyWeakref_ClearRef()

#include "pydtrace.h"


// enable the "mark alive" pass of GC
#define GC_ENABLE_MARK_ALIVE 1

// if true, enable the use of "prefetch" CPU instructions
#define GC_ENABLE_PREFETCH_INSTRUCTIONS 1

// include additional roots in "mark alive" pass
#define GC_MARK_ALIVE_EXTRA_ROOTS 1

// include Python stacks as set of known roots
#define GC_MARK_ALIVE_STACKS 1

// Experimental: record how long each phase of a collection takes and print
// it to stderr when the PYTHON_GC_PHASE_TRACE environment variable is set.
// #define GC_PHASE_TRACE 1     // (off: compile-time only)


#ifdef Py_GIL_DISABLED

typedef struct _gc_runtime_state GCState;

#ifdef Py_DEBUG
#  define GC_DEBUG
#endif

// Each thread buffers the count of allocated objects in a thread-local
// variable up to +/- this amount to reduce the overhead of updating
// the global count.
#define LOCAL_ALLOC_COUNT_THRESHOLD 512

// Automatically choose the generation that needs collecting.
#define GENERATION_AUTO (-1)

// A linked list of objects using the `ob_tid` field as the next pointer.
// The linked list pointers are distinct from any real thread ids, because the
// thread ids returned by _Py_ThreadId() are also pointers to distinct objects.
// No thread will confuse its own id with a linked list pointer.
struct worklist {
    uintptr_t head;
};

struct worklist_iter {
    uintptr_t *ptr;   // pointer to current object
    uintptr_t *next;  // next value of ptr
};

struct visitor_args {
    size_t offset;  // offset of PyObject from start of block
};

// Pages to move between the young and old heaps (generational collection),
// decided by the scan and moved when it is done.
struct gc_page_list {
    mi_page_t **pages;
    Py_ssize_t n, cap;
};

#ifdef GC_PHASE_TRACE
enum gc_phase {
    GC_PHASE_STW1_WAIT,     // waiting for other threads to stop
    GC_PHASE_MERGE,         // merge per-thread and queued refcounts
    GC_PHASE_DELAYED,       // process QSBR delayed frees
    GC_PHASE_TRIM_PENDING,  // free empty heap pages (deferred from last collection)
    GC_PHASE_MARK_ALIVE,    // mark objects reachable from roots
    GC_PHASE_UPDATE_REFS,   // heap walk: compute gc_refs
    GC_PHASE_MARK_HEAP,     // heap walk: propagate reachability
    GC_PHASE_SCAN_HEAP,     // heap walk: collect unreachable, restore ob_tid
    GC_PHASE_WEAKREFS,      // find weakref callbacks
    GC_PHASE_FINALIZE,      // world running: callbacks and finalizers
    GC_PHASE_STW2_WAIT,
    GC_PHASE_RESURRECT,     // handle resurrected objects, clear weakrefs
    GC_PHASE_DELETE,        // world running: tp_clear and deallocation
    GC_PHASE_TRIM,          // stop the world and free empty heap pages
    GC_PHASE_PURGE,         // world running: return freed segments to the OS
    GC_PHASE_COUNT
};

static const char *gc_phase_names[GC_PHASE_COUNT] = {
    "stw1_wait", "merge", "delayed", "trim_pending", "mark_alive", "update_refs",
    "mark_heap", "scan_heap", "weakrefs", "finalize", "stw2_wait",
    "resurrect", "delete", "trim", "purge",
};
#endif

// A chunked array of objects (here: the garbage of a collection, split into
// chunks that threads help free, see "GC assist").
#define GC_OBJ_CHUNK_SIZE 1024

struct gc_obj_chunk {
    struct gc_obj_chunk *next;
    Py_ssize_t n;
    PyObject *items[GC_OBJ_CHUNK_SIZE];
};

struct gc_obj_array {
    struct gc_obj_chunk *head;
    struct gc_obj_chunk *tail;
    // Set if a chunk allocation failed; the array is then incomplete and
    // the collector falls back to walking the heaps.
    bool failed;
};

#define GC_OBJ_ARRAY_FOR_EACH(arr, chunk, i, op) \
    for ((chunk) = (arr)->head; (chunk) != NULL; (chunk) = (chunk)->next) \
        for ((i) = 0; (i) < (chunk)->n && ((op) = (chunk)->items[(i)], 1); (i)++)

static void
gc_obj_array_push(struct gc_obj_array *arr, PyObject *op)
{
    struct gc_obj_chunk *tail = arr->tail;
    if (tail == NULL || tail->n == GC_OBJ_CHUNK_SIZE) {
        if (arr->failed) {
            return;
        }
        // Must not use mimalloc-managed heaps being walked; see
        // _PyObjectStackChunk_New().
        struct gc_obj_chunk *chunk = PyMem_RawMalloc(sizeof(*chunk));
        if (chunk == NULL) {
            arr->failed = true;
            return;
        }
        chunk->next = NULL;
        chunk->n = 0;
        if (tail == NULL) {
            arr->head = chunk;
        }
        else {
            tail->next = chunk;
        }
        arr->tail = tail = chunk;
    }
    tail->items[tail->n++] = op;
}

static void
gc_obj_array_clear(struct gc_obj_array *arr)
{
    struct gc_obj_chunk *chunk = arr->head;
    while (chunk != NULL) {
        struct gc_obj_chunk *next = chunk->next;
        PyMem_RawFree(chunk);
        chunk = next;
    }
    arr->head = arr->tail = NULL;
    arr->failed = false;
}

// Per-collection state
struct collection_state {
    struct visitor_args base;
    PyInterpreterState *interp;
    GCState *gcstate;
    _PyGC_Reason reason;
    // GH-129236: If we see an active frame without a valid stack pointer,
    // we can't collect objects with deferred references because we may not
    // see all references.
    int skip_deferred_objects;
    Py_ssize_t collected;
    Py_ssize_t uncollectable;
    Py_ssize_t candidates;
    Py_ssize_t long_lived_total;
    struct worklist unreachable;
    struct worklist legacy_finalizers;
    struct worklist wrcb_to_call;
    struct worklist objs_to_decref;
    // Young collection: only objects on pages of the young heaps are
    // candidates (see "Generational collection" below).
    bool young;
    // Objects that survived this collection and were marked old by it.
    Py_ssize_t aged;
    // The candidates of a young collection, in heap order: its pages are
    // walked once, and the later phases only go over the candidates.
    PyObject **young_objs;
    Py_ssize_t young_n, young_cap;
    bool young_oom;
    Py_ssize_t scan_page_old;       // old objects on the page being walked
    // Generational collection: the page the scan is in, how many of its
    // blocks are in use and how many of those are garbage; the pages to
    // promote and demote, and their surviving blocks.
    mi_page_t *scan_page;
    Py_ssize_t scan_page_used;
    Py_ssize_t scan_page_garbage;
    struct gc_page_list promote, demote;
    Py_ssize_t promote_live, demote_live;
    // The unreachable objects in scan order, recorded by scan_object().
    // Used instead of the `unreachable` worklist to free the garbage when no
    // object can be resurrected (see delete_garbage_parallel()).
    struct gc_obj_array garbage;
    // Set by scan_object() if an unreachable object is a weakref or
    // has weakrefs, or has a finalizer that hasn't run yet.  If neither is
    // set, no unreachable object can be resurrected.
    bool has_weakrefs;
    bool has_finalizers;
    // Number of objects collected by other threads (see gc_assist_delete).
    Py_ssize_t assisted;
#ifdef GC_PHASE_TRACE
    PyTime_t phase_last;
    PyTime_t phase_ns[GC_PHASE_COUNT];
#endif
};

#ifdef GC_PHASE_TRACE
// Charge the time since the previous call to `phase`.
static void
gc_phase_done(struct collection_state *state, enum gc_phase phase)
{
    PyTime_t now;
    (void)PyTime_PerfCounterRaw(&now);
    state->phase_ns[phase] += now - state->phase_last;
    state->phase_last = now;
}

static void
gc_phase_report(struct collection_state *state)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("PYTHON_GC_PHASE_TRACE") != NULL;
    }
    if (!enabled) {
        return;
    }
    PyTime_t *ns = state->phase_ns;
    PyTime_t stw1 = 0, stw2 = 0;
    for (int i = GC_PHASE_STW1_WAIT; i <= GC_PHASE_WEAKREFS; i++) {
        stw1 += ns[i];
    }
    stw2 = ns[GC_PHASE_STW2_WAIT] + ns[GC_PHASE_RESURRECT];
    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
                       "gcphase candidates=%zd collected=%zd assisted=%zd "
                       "live=%zd stw1=%.3f stw2=%.3f",
                       state->candidates, state->collected, state->assisted,
                       state->long_lived_total, stw1 / 1e6, stw2 / 1e6);
    for (int i = 0; i < GC_PHASE_COUNT && len < (int)sizeof(buf); i++) {
        len += snprintf(buf + len, sizeof(buf) - len, " %s=%.3f",
                        gc_phase_names[i], ns[i] / 1e6);
    }
    fprintf(stderr, "%s\n", buf);
}
#  define GC_PHASE_DONE(state, phase) gc_phase_done((state), (phase))
#else
#  define GC_PHASE_DONE(state, phase) ((void)0)
#endif

// iterate over a worklist
#define WORKSTACK_FOR_EACH(stack, op) \
    for ((op) = (PyObject *)(stack)->head; (op) != NULL; (op) = (PyObject *)(op)->ob_tid)

// iterate over a worklist with support for removing the current object
#define WORKSTACK_FOR_EACH_ITER(stack, iter, op) \
    for (worklist_iter_init((iter), &(stack)->head), (op) = (PyObject *)(*(iter)->ptr); \
         (op) != NULL; \
         worklist_iter_init((iter), (iter)->next), (op) = (PyObject *)(*(iter)->ptr))

static void
worklist_push(struct worklist *worklist, PyObject *op)
{
    assert(op->ob_tid == 0);
    op->ob_tid = worklist->head;
    worklist->head = (uintptr_t)op;
}

static PyObject *
worklist_pop(struct worklist *worklist)
{
    PyObject *op = (PyObject *)worklist->head;
    if (op != NULL) {
        worklist->head = op->ob_tid;
        _Py_atomic_store_uintptr_relaxed(&op->ob_tid, 0);
    }
    return op;
}

static void
worklist_iter_init(struct worklist_iter *iter, uintptr_t *next)
{
    iter->ptr = next;
    PyObject *op = (PyObject *)*(iter->ptr);
    if (op) {
        iter->next = &op->ob_tid;
    }
}

static void
worklist_remove(struct worklist_iter *iter)
{
    PyObject *op = (PyObject *)*(iter->ptr);
    *(iter->ptr) = op->ob_tid;
    op->ob_tid = 0;
    iter->next = iter->ptr;
}

static inline int
gc_has_bit(PyObject *op, uint8_t bit)
{
    return (op->ob_gc_bits & bit) != 0;
}

static inline void
gc_set_bit(PyObject *op, uint8_t bit)
{
    op->ob_gc_bits |= bit;
}

static inline void
gc_clear_bit(PyObject *op, uint8_t bit)
{
    op->ob_gc_bits &= ~bit;
}

static inline int
gc_is_frozen(PyObject *op)
{
    return gc_has_bit(op, _PyGC_BITS_FROZEN);
}

static inline int
gc_is_unreachable(PyObject *op)
{
    return gc_has_bit(op, _PyGC_BITS_UNREACHABLE);
}

static inline void
gc_set_unreachable(PyObject *op)
{
    gc_set_bit(op, _PyGC_BITS_UNREACHABLE);
}

static inline void
gc_clear_unreachable(PyObject *op)
{
    gc_clear_bit(op, _PyGC_BITS_UNREACHABLE);
}

static inline int
gc_is_alive(PyObject *op)
{
    return gc_has_bit(op, _PyGC_BITS_ALIVE);
}

#ifdef GC_ENABLE_MARK_ALIVE
static inline void
gc_set_alive(PyObject *op)
{
    gc_set_bit(op, _PyGC_BITS_ALIVE);
}
#endif

static inline void
gc_clear_alive(PyObject *op)
{
    gc_clear_bit(op, _PyGC_BITS_ALIVE);
}

// Initialize the `ob_tid` field to zero if the object is not already
// initialized as unreachable.
static void
gc_maybe_init_refs(PyObject *op)
{
    if (!gc_is_unreachable(op)) {
        assert(!gc_is_alive(op));
        gc_set_unreachable(op);
        op->ob_tid = 0;
    }
}

static inline Py_ssize_t
gc_get_refs(PyObject *op)
{
    return (Py_ssize_t)op->ob_tid;
}

static inline void
gc_add_refs(PyObject *op, Py_ssize_t refs)
{
    assert(_PyObject_GC_IS_TRACKED(op));
    op->ob_tid += refs;
}

static inline void
gc_decref(PyObject *op)
{
    op->ob_tid -= 1;
}

static Py_ssize_t
merge_refcount(PyObject *op, Py_ssize_t extra)
{
    assert(_PyInterpreterState_GET()->stoptheworld.world_stopped);

    Py_ssize_t refcount = Py_REFCNT(op);
    refcount += extra;

#ifdef Py_REF_DEBUG
    _Py_AddRefTotal(_PyThreadState_GET(), extra);
#endif

    // No atomics necessary; all other threads in this interpreter are paused.
    op->ob_tid = 0;
    op->ob_ref_local = 0;
    op->ob_ref_shared = _Py_REF_SHARED(refcount, _Py_REF_MERGED);
    return refcount;
}

static void
frame_disable_deferred_refcounting(_PyInterpreterFrame *frame)
{
    // Convert locals, variables, and the executable object to strong
    // references from (possibly) deferred references.
    assert(frame->stackpointer != NULL);
    assert(frame->owner == FRAME_OWNED_BY_FRAME_OBJECT ||
           frame->owner == FRAME_OWNED_BY_GENERATOR);

    frame->f_executable = PyStackRef_AsStrongReference(frame->f_executable);

    if (frame->owner == FRAME_OWNED_BY_GENERATOR) {
        PyGenObject *gen = _PyGen_GetGeneratorFromFrame(frame);
        if (gen->gi_frame_state == FRAME_CLEARED) {
            // gh-124068: if the generator is cleared, then most fields other
            // than f_executable are not valid.
            return;
        }
    }

    frame->f_funcobj = PyStackRef_AsStrongReference(frame->f_funcobj);
    for (_PyStackRef *ref = frame->localsplus; ref < frame->stackpointer; ref++) {
        if (!PyStackRef_IsNullOrInt(*ref) && !PyStackRef_RefcountOnObject(*ref)) {
            *ref = PyStackRef_AsStrongReference(*ref);
        }
    }
}

static void
disable_deferred_refcounting(PyObject *op)
{
    if (_PyObject_HasDeferredRefcount(op)) {
        op->ob_gc_bits &= ~_PyGC_BITS_DEFERRED;
        op->ob_ref_shared -= _Py_REF_SHARED(_Py_REF_DEFERRED, 0);
        merge_refcount(op, 0);

        // Heap types and code objects also use per-thread refcounting, which
        // should also be disabled when we turn off deferred refcounting.
        _PyObject_DisablePerThreadRefcounting(op);
    }

    // Generators and frame objects may contain deferred references to other
    // objects. If the pointed-to objects are part of cyclic trash, we may
    // have disabled deferred refcounting on them and need to ensure that we
    // use strong references, in case the generator or frame object is
    // resurrected by a finalizer.
    if (PyGen_CheckExact(op) || PyCoro_CheckExact(op) || PyAsyncGen_CheckExact(op)) {
        frame_disable_deferred_refcounting(&((PyGenObject *)op)->gi_iframe);
    }
    else if (PyFrame_Check(op)) {
        frame_disable_deferred_refcounting(((PyFrameObject *)op)->f_frame);
    }
}

static void
gc_restore_tid(PyObject *op)
{
    assert(_PyInterpreterState_GET()->stoptheworld.world_stopped);
    mi_segment_t *segment = _mi_ptr_segment(op);
    if (_Py_REF_IS_MERGED(op->ob_ref_shared)) {
        op->ob_tid = 0;
    }
    else {
        // NOTE: may change ob_tid if the object was re-initialized by
        // a different thread or its segment was abandoned and reclaimed.
        // The segment thread id might be zero, in which case we should
        // ensure the refcounts are now merged.
        op->ob_tid = segment->thread_id;
        if (op->ob_tid == 0) {
            merge_refcount(op, 0);
        }
    }
}

static void
gc_restore_refs(PyObject *op)
{
    if (gc_is_unreachable(op)) {
        assert(!gc_is_alive(op));
        gc_restore_tid(op);
        gc_clear_unreachable(op);
    }
    else {
        gc_clear_alive(op);
    }
}

// Given a mimalloc memory block return the PyObject stored in it or NULL if
// the block is not allocated or the object is not tracked or is immortal.
static PyObject *
op_from_block(void *block, void *arg, bool include_frozen)
{
    struct visitor_args *a = arg;
    if (block == NULL) {
        return NULL;
    }
    PyObject *op = (PyObject *)((char*)block + a->offset);
    assert(PyObject_IS_GC(op));
    if (!_PyObject_GC_IS_TRACKED(op)) {
        return NULL;
    }
    if (!include_frozen && gc_is_frozen(op)) {
        return NULL;
    }
    return op;
}

// As above but returns untracked and frozen objects as well.
static PyObject *
op_from_block_all_gc(void *block, void *arg)
{
    struct visitor_args *a = arg;
    if (block == NULL) {
        return NULL;
    }
    PyObject *op = (PyObject *)((char*)block + a->offset);
    assert(PyObject_IS_GC(op));
    return op;
}

static int
gc_visit_heaps_lock_held(PyInterpreterState *interp, mi_block_visit_fun *visitor,
                         struct visitor_args *arg, bool young_only)
{
    // The heaps to visit for objects without and with pre-header.
    int heaps[2][2] = {
        {_Py_MIMALLOC_HEAP_GC_YOUNG, _Py_MIMALLOC_HEAP_GC},
        {_Py_MIMALLOC_HEAP_GC_PRE_YOUNG, _Py_MIMALLOC_HEAP_GC_PRE},
    };
    int nheaps = young_only ? 1 : 2;

    // Offset of PyObject header from start of memory block.
    Py_ssize_t offset_base = 0;
    if (_PyMem_DebugEnabled()) {
        // The debug allocator adds two words at the beginning of each block.
        offset_base += 2 * sizeof(size_t);
    }

    // Objects with Py_TPFLAGS_PREHEADER have two extra fields
    Py_ssize_t offset_pre = offset_base + 2 * sizeof(PyObject*);

    // visit each thread's heaps for GC objects
    _Py_FOR_EACH_TSTATE_UNLOCKED(interp, p) {
        struct _mimalloc_thread_state *m = &((_PyThreadStateImpl *)p)->mimalloc;
        if (!_Py_atomic_load_int(&m->initialized)) {
            // The thread may not have called tstate_mimalloc_bind() yet.
            continue;
        }

        for (int h = 0; h < nheaps; h++) {
            arg->offset = offset_base;
            if (!mi_heap_visit_blocks(&m->heaps[heaps[0][h]], true,
                                      visitor, arg)) {
                return -1;
            }
            arg->offset = offset_pre;
            if (!mi_heap_visit_blocks(&m->heaps[heaps[1][h]], true,
                                      visitor, arg)) {
                return -1;
            }
        }
    }

    // visit blocks in the per-interpreter abandoned pool (from dead threads)
    mi_abandoned_pool_t *pool = &interp->mimalloc.abandoned_pool;
    for (int h = 0; h < nheaps; h++) {
        arg->offset = offset_base;
        if (!_mi_abandoned_pool_visit_blocks(pool, heaps[0][h], true,
                                             visitor, arg)) {
            return -1;
        }
        arg->offset = offset_pre;
        if (!_mi_abandoned_pool_visit_blocks(pool, heaps[1][h], true,
                                             visitor, arg)) {
            return -1;
        }
    }
    return 0;
}

// Visits all GC objects in the interpreter's heaps.
// NOTE: It is not safe to allocate or free any mimalloc managed memory while
// this function is running.
static int
gc_visit_heaps(PyInterpreterState *interp, mi_block_visit_fun *visitor,
               struct visitor_args *arg)
{
    // Other threads in the interpreter must be paused so that we can safely
    // traverse their heaps.
    assert(interp->stoptheworld.world_stopped);

    int err;
    HEAD_LOCK(&_PyRuntime);
    err = gc_visit_heaps_lock_held(interp, visitor, arg, false);
    HEAD_UNLOCK(&_PyRuntime);
    return err;
}

static inline void
gc_visit_stackref(_PyStackRef stackref)
{
    if (!PyStackRef_IsNullOrInt(stackref) && !PyStackRef_RefcountOnObject(stackref)) {
        PyObject *obj = PyStackRef_AsPyObjectBorrow(stackref);
        if (_PyObject_GC_IS_TRACKED(obj) && !gc_is_frozen(obj)) {
            gc_add_refs(obj, 1);
        }
    }
}

// Add 1 to the gc_refs for every deferred reference on each thread's stack.
static void
gc_visit_thread_stacks(PyInterpreterState *interp, struct collection_state *state)
{
    _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
        _PyCStackRef *c_ref = ((_PyThreadStateImpl *)p)->c_stack_refs;
        while (c_ref != NULL) {
            gc_visit_stackref(c_ref->ref);
            c_ref = c_ref->next;
        }

        for (_PyInterpreterFrame *f = p->current_frame; f != NULL; f = f->previous) {
            if (f->owner >= FRAME_OWNED_BY_INTERPRETER) {
                continue;
            }

            _PyStackRef *top = f->stackpointer;
            if (top == NULL) {
                // GH-129236: The stackpointer may be NULL in cases where
                // the GC is run during a PyStackRef_CLOSE() call. Skip this
                // frame and don't collect objects with deferred references.
                state->skip_deferred_objects = 1;
                continue;
            }

            gc_visit_stackref(f->f_executable);
            while (top != f->localsplus) {
                --top;
                gc_visit_stackref(*top);
            }
        }
    }
    _Py_FOR_EACH_TSTATE_END(interp);
}

// Untrack objects that can never create reference cycles.
// Return true if the object was untracked.
static bool
gc_maybe_untrack(PyObject *op)
{
    if (_PyObject_HasDeferredRefcount(op)) {
        // deferred refcounting only works if the object is tracked
        return false;
    }
    // Currently we only check for tuples containing only non-GC objects.  In
    // theory we could check other immutable objects that contain references
    // to non-GC objects.
    if (PyTuple_CheckExact(op)) {
        _PyTuple_MaybeUntrack(op);
        if (!_PyObject_GC_IS_TRACKED(op)) {
            return true;
        }
    }
    return false;
}

#ifdef GC_ENABLE_MARK_ALIVE

// prefetch buffer and stack //////////////////////////////////

// The buffer is a circular FIFO queue of PyObject pointers.  We take
// care to not dereference these pointers until they are taken out of
// the buffer.  A prefetch CPU instruction is issued when a pointer is
// put into the buffer.  If all is working as expected, there will be
// enough time between the enqueue and dequeue so that the needed memory
// for the object, most importantly ob_gc_bits and ob_type words, will
// already be in the CPU cache.
#define BUFFER_SIZE 256
#define BUFFER_HI 16
#define BUFFER_LO 8
#define BUFFER_MASK (BUFFER_SIZE - 1)

// the buffer size must be an exact power of two
static_assert(BUFFER_SIZE > 0 && !(BUFFER_SIZE & BUFFER_MASK),
              "Invalid BUFFER_SIZE, must be power of 2");
// the code below assumes these relationships are true
static_assert(BUFFER_HI < BUFFER_SIZE &&
              BUFFER_LO < BUFFER_HI &&
              BUFFER_LO > 0,
              "Invalid prefetch buffer level settings.");

// Prefetch intructions will fetch the line of data from memory that
// contains the byte specified with the source operand to a location in
// the cache hierarchy specified by a locality hint.  The instruction
// is only a hint and the CPU is free to ignore it.  Instructions and
// behaviour are CPU specific but the definitions of locality hints
// below are mostly consistent.
//
// * T0 (temporal data) prefetch data into all levels of the cache hierarchy.
//
// * T1 (temporal data with respect to first level cache) prefetch data into
//   level 2 cache and higher.
//
// * T2 (temporal data with respect to second level cache) prefetch data into
//   level 3 cache and higher, or an implementation-specific choice.
//
// * NTA (non-temporal data with respect to all cache levels) prefetch data into
//   non-temporal cache structure and into a location close to the processor,
//   minimizing cache pollution.

#if defined(__GNUC__) || defined(__clang__)
    #define PREFETCH_T0(ptr)  __builtin_prefetch(ptr, 0, 3)
    #define PREFETCH_T1(ptr)  __builtin_prefetch(ptr, 0, 2)
    #define PREFETCH_T2(ptr)  __builtin_prefetch(ptr, 0, 1)
    #define PREFETCH_NTA(ptr)  __builtin_prefetch(ptr, 0, 0)
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_I86)) && !defined(_M_ARM64EC)
    #include <mmintrin.h>
    #define PREFETCH_T0(ptr)  _mm_prefetch((const char*)(ptr), _MM_HINT_T0)
    #define PREFETCH_T1(ptr)  _mm_prefetch((const char*)(ptr), _MM_HINT_T1)
    #define PREFETCH_T2(ptr)  _mm_prefetch((const char*)(ptr), _MM_HINT_T2)
    #define PREFETCH_NTA(ptr)  _mm_prefetch((const char*)(ptr), _MM_HINT_NTA)
#elif defined (__aarch64__)
    #define PREFETCH_T0(ptr)  \
        do { __asm__ __volatile__("prfm pldl1keep, %0" ::"Q"(*(ptr))); } while (0)
    #define PREFETCH_T1(ptr)  \
        do { __asm__ __volatile__("prfm pldl2keep, %0" ::"Q"(*(ptr))); } while (0)
    #define PREFETCH_T2(ptr)  \
        do { __asm__ __volatile__("prfm pldl3keep, %0" ::"Q"(*(ptr))); } while (0)
    #define PREFETCH_NTA(ptr)  \
        do { __asm__ __volatile__("prfm pldl1strm, %0" ::"Q"(*(ptr))); } while (0)
#else
    #define PREFETCH_T0(ptr) do { (void)(ptr); } while (0)  /* disabled */
    #define PREFETCH_T1(ptr) do { (void)(ptr); } while (0)  /* disabled */
    #define PREFETCH_T2(ptr) do { (void)(ptr); } while (0)  /* disabled */
    #define PREFETCH_NTA(ptr) do { (void)(ptr); } while (0)  /* disabled */
#endif

#ifdef GC_ENABLE_PREFETCH_INSTRUCTIONS
    #define prefetch(ptr) PREFETCH_T1(ptr)
#else
    #define prefetch(ptr)
#endif

// a contigous sequence of PyObject pointers, can contain NULLs
typedef struct {
    PyObject **start;
    PyObject **end;
} gc_span_t;

typedef struct {
    Py_ssize_t size;
    Py_ssize_t capacity;
    gc_span_t *stack;
} gc_span_stack_t;

typedef struct {
    unsigned int in;
    unsigned int out;
    _PyObjectStack stack;
    gc_span_stack_t spans;
    PyObject *buffer[BUFFER_SIZE];
    bool use_prefetch;
} gc_mark_args_t;


// Returns number of entries in buffer
static inline unsigned int
gc_mark_buffer_len(gc_mark_args_t *args)
{
    return args->in - args->out;
}

// Returns number of free entry slots in buffer
#ifndef NDEBUG
static inline unsigned int
gc_mark_buffer_avail(gc_mark_args_t *args)
{
    return BUFFER_SIZE - gc_mark_buffer_len(args);
}
#endif

static inline bool
gc_mark_buffer_is_empty(gc_mark_args_t *args)
{
    return args->in == args->out;
}

static inline bool
gc_mark_buffer_is_full(gc_mark_args_t *args)
{
    return gc_mark_buffer_len(args) == BUFFER_SIZE;
}

static inline PyObject *
gc_mark_buffer_pop(gc_mark_args_t *args)
{
    assert(!gc_mark_buffer_is_empty(args));
    PyObject *op = args->buffer[args->out & BUFFER_MASK];
    args->out++;
    return op;
}

// Called when there is space in the buffer for the object.  Issue the
// prefetch instruction and add it to the end of the buffer.
static inline void
gc_mark_buffer_push(PyObject *op, gc_mark_args_t *args)
{
    assert(!gc_mark_buffer_is_full(args));
    prefetch(op);
    args->buffer[args->in & BUFFER_MASK] = op;
    args->in++;
}

// Called when we run out of space in the buffer or if the prefetching
// is disabled. The object will be pushed on the gc_mark_args.stack.
static int
gc_mark_stack_push(_PyObjectStack *ms, PyObject *op)
{
    if (_PyObjectStack_Push(ms, op) < 0) {
        return -1;
    }
    return 0;
}

static int
gc_mark_span_push(gc_span_stack_t *ss, PyObject **start, PyObject **end)
{
    if (start == end) {
        return 0;
    }
    if (ss->size >= ss->capacity) {
        if (ss->capacity == 0) {
            ss->capacity = 256;
        }
        else {
            ss->capacity *= 2;
        }
        gc_span_t *new_stack = (gc_span_t *)PyMem_Realloc(ss->stack, ss->capacity * sizeof(gc_span_t));
        if (new_stack == NULL) {
            return -1;
        }
        ss->stack = new_stack;
    }
    assert(end > start);
    ss->stack[ss->size].start = start;
    ss->stack[ss->size].end = end;
    ss->size++;
    return 0;
}

static int
gc_mark_enqueue_no_buffer(PyObject *op, gc_mark_args_t *args)
{
    if (op == NULL) {
        return 0;
    }
    if (!gc_has_bit(op,  _PyGC_BITS_TRACKED)) {
        return 0;
    }
    if (gc_is_alive(op)) {
        return 0; // already visited this object
    }
    if (gc_maybe_untrack(op)) {
        return 0; // was untracked, don't visit it
    }

    // Need to call tp_traverse on this object. Add to stack and mark it
    // alive so we don't traverse it a second time.
    gc_set_alive(op);
    if (_PyObjectStack_Push(&args->stack, op) < 0) {
        return -1;
    }
    return 0;
}

static inline int
gc_mark_enqueue_no_buffer_visitproc(PyObject *op, void *args)
{
    return gc_mark_enqueue_no_buffer(op, (gc_mark_args_t *)args);
}

static int
gc_mark_enqueue_buffer(PyObject *op, gc_mark_args_t *args)
{
    assert(op != NULL);
    if (!gc_mark_buffer_is_full(args)) {
        gc_mark_buffer_push(op, args);
        return 0;
    }
    else {
        return gc_mark_stack_push(&args->stack, op);
    }
}

static inline int
gc_mark_enqueue_buffer_visitproc(PyObject *op, void *args)
{
    return gc_mark_enqueue_buffer(op, (gc_mark_args_t *)args);
}

// Called when we find an object that needs to be marked alive (either from a
// root or from calling tp_traverse).
static int
gc_mark_enqueue(PyObject *op, gc_mark_args_t *args)
{
    if (args->use_prefetch) {
        return gc_mark_enqueue_buffer(op, args);
    }
    else {
        return gc_mark_enqueue_no_buffer(op, args);
    }
}

// Called when we have a contigous sequence of PyObject pointers, either
// a tuple or list object.  This will add the items to the buffer if there
// is space for them all otherwise push a new "span" on the span stack.  Using
// spans has the advantage of not creating a deep _PyObjectStack stack when
// dealing with long sequences.  Those sequences will be processed in smaller
// chunks by the gc_prime_from_spans() function.
static int
gc_mark_enqueue_span(PyObject **item, Py_ssize_t size, gc_mark_args_t *args)
{
    Py_ssize_t used = gc_mark_buffer_len(args);
    Py_ssize_t free = BUFFER_SIZE - used;
    if (free >= size) {
        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject *op = item[i];
            if (op == NULL) {
                continue;
            }
            gc_mark_buffer_push(op, args);
        }
    }
    else {
        assert(size > 0);
        PyObject **end = &item[size];
        if (gc_mark_span_push(&args->spans, item, end) < 0) {
            return -1;
        }
    }
    return 0;
}

static bool
gc_clear_alive_bits(const mi_heap_t *heap, const mi_heap_area_t *area,
                    void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }
    if (gc_is_alive(op)) {
        gc_clear_alive(op);
    }
    return true;
}

static int
gc_mark_traverse_list(PyObject *self, void *args)
{
    PyListObject *list = (PyListObject *)self;
    if (list->ob_item == NULL) {
        return 0;
    }
    if (gc_mark_enqueue_span(list->ob_item, PyList_GET_SIZE(list), args) < 0) {
        return -1;
    }
    return 0;
}

static int
gc_mark_traverse_tuple(PyObject *self, void *args)
{
    _PyTuple_MaybeUntrack(self);
    if (!gc_has_bit(self,  _PyGC_BITS_TRACKED)) {
        gc_clear_alive(self);
        return 0;
    }
    PyTupleObject *tuple = _PyTuple_CAST(self);
    if (gc_mark_enqueue_span(tuple->ob_item, Py_SIZE(tuple), args) < 0) {
        return -1;
    }
    return 0;
}

static void
gc_abort_mark_alive(PyInterpreterState *interp,
                    struct collection_state *state,
                    gc_mark_args_t *args)
{
    // We failed to allocate memory while doing the "mark alive" phase.
    // In that case, free the memory used for marking state and make
    // sure that no objects have the alive bit set.
    _PyObjectStack_Clear(&args->stack);
    if (args->spans.stack != NULL) {
        PyMem_Free(args->spans.stack);
    }
    gc_visit_heaps(interp, &gc_clear_alive_bits, &state->base);
}

#ifdef GC_MARK_ALIVE_STACKS
static int
gc_visit_stackref_mark_alive(gc_mark_args_t *args, _PyStackRef stackref)
{
    if (!PyStackRef_IsNullOrInt(stackref)) {
        PyObject *op = PyStackRef_AsPyObjectBorrow(stackref);
        if (gc_mark_enqueue(op, args) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
gc_visit_thread_stacks_mark_alive(PyInterpreterState *interp, gc_mark_args_t *args)
{
    int err = 0;
    _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
        for (_PyInterpreterFrame *f = p->current_frame; f != NULL; f = f->previous) {
            if (f->owner >= FRAME_OWNED_BY_INTERPRETER) {
                continue;
            }

            if (f->stackpointer == NULL) {
                // GH-129236: The stackpointer may be NULL in cases where
                // the GC is run during a PyStackRef_CLOSE() call. Skip this
                // frame for now.
                continue;
            }

            _PyStackRef *top = f->stackpointer;
            if (gc_visit_stackref_mark_alive(args, f->f_executable) < 0) {
                err = -1;
                goto exit;
            }
            while (top != f->localsplus) {
                --top;
                if (gc_visit_stackref_mark_alive(args, *top) < 0) {
                    err = -1;
                    goto exit;
                }
            }
        }
    }
exit:
    _Py_FOR_EACH_TSTATE_END(interp);
    return err;
}
#endif // GC_MARK_ALIVE_STACKS
#endif // GC_ENABLE_MARK_ALIVE

static bool gc_is_young_object(PyObject *op);
static int gc_generational_enabled(void);
static void gc_scan_page_done(struct collection_state *state);

static void
queue_untracked_obj_decref(PyObject *op, struct collection_state *state)
{
    assert(Py_REFCNT(op) == 0);
    if (state->young && _PyObject_GC_IS_TRACKED(op) && !gc_is_frozen(op) &&
        !gc_is_young_object(op))
    {
        // An old object that this young collection won't look at: release
        // it after the pause like an untracked one.
        op->ob_ref_shared += (1 << _Py_REF_SHARED_SHIFT);
#ifdef Py_REF_DEBUG
        _Py_IncRefTotal(_PyThreadState_GET());
#endif
        worklist_push(&state->objs_to_decref, op);
        return;
    }
    // gh-142975: We have to treat frozen objects as untracked in this function
    // or else they might be picked up in a future collection, which breaks the
    // assumption that all incoming objects have a non-zero reference count.
    if (!_PyObject_GC_IS_TRACKED(op) || gc_is_frozen(op)) {
        // GC objects with zero refcount are handled subsequently by the
        // GC as if they were cyclic trash, but we have to handle dead
        // non-GC objects here. Add one to the refcount so that we can
        // decref and deallocate the object once we start the world again.
        op->ob_ref_shared += (1 << _Py_REF_SHARED_SHIFT);
#ifdef Py_REF_DEBUG
        _Py_IncRefTotal(_PyThreadState_GET());
#endif
        worklist_push(&state->objs_to_decref, op);
    }

}

static void
merge_queued_objects(_PyThreadStateImpl *tstate, struct collection_state *state)
{
    struct _brc_thread_state *brc = &tstate->brc;
    _PyObjectStack_Merge(&brc->local_objects_to_merge, &brc->objects_to_merge);

    PyObject *op;
    while ((op = _PyObjectStack_Pop(&brc->local_objects_to_merge)) != NULL) {
        // Subtract one when merging because the queue had a reference.
        Py_ssize_t refcount = merge_refcount(op, -1);

        if (refcount == 0) {
            queue_untracked_obj_decref(op, state);
        }
    }
}

static void
queue_freed_object(PyObject *obj, void *arg)
{
    queue_untracked_obj_decref(obj, arg);
}

static void
process_delayed_frees(PyInterpreterState *interp, struct collection_state *state)
{
    // While we are in a "stop the world" pause, we can observe the latest
    // write sequence by advancing the write sequence immediately.
    _Py_qsbr_advance(&interp->qsbr);
    _PyThreadStateImpl *current_tstate = (_PyThreadStateImpl *)_PyThreadState_GET();
    _Py_qsbr_quiescent_state(current_tstate->qsbr);

    // Merge the queues from other threads into our own queue so that we can
    // process all of the pending delayed free requests at once.
    _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
        _PyThreadStateImpl *other = (_PyThreadStateImpl *)p;
        if (other != current_tstate) {
            llist_concat(&current_tstate->mem_free_queue, &other->mem_free_queue);
        }
    }
    _Py_FOR_EACH_TSTATE_END(interp);

    _PyMem_ProcessDelayedNoDealloc((PyThreadState *)current_tstate, queue_freed_object, state);
}

// Subtract an incoming reference from the computed "gc_refs" refcount.
static int
visit_decref(PyObject *op, void *arg)
{
    if (_PyObject_GC_IS_TRACKED(op)
        && !_Py_IsImmortal(op)
        && !gc_is_frozen(op)
        && !gc_is_alive(op))
    {
        // If update_refs hasn't reached this object yet, mark it
        // as (tentatively) unreachable and initialize ob_tid to zero.
        gc_maybe_init_refs(op);
        gc_decref(op);
    }
    return 0;
}

// Compute the number of external references to objects in the heap
// by subtracting internal references from the refcount. The difference is
// computed in the ob_tid field (we restore it later).
static bool
update_refs(const mi_heap_t *heap, const mi_heap_area_t *area,
            void *block, size_t block_size, void *args)
{
    struct collection_state *state = (struct collection_state *)args;
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }

    // Exclude immortal objects from garbage collection
    if (_Py_IsImmortal(op)) {
        op->ob_tid = 0;
        _PyObject_GC_UNTRACK(op);
        gc_clear_unreachable(op);
        return true;
    }
    // Marked objects count as candidates, immortals don't:
    state->candidates++;
    if (gc_is_alive(op)) {
        return true;
    }

    Py_ssize_t refcount = Py_REFCNT(op);
    if (_PyObject_HasDeferredRefcount(op)) {
        refcount -= _Py_REF_DEFERRED;
    }
    _PyObject_ASSERT(op, refcount >= 0);

    if (refcount > 0) {
        if (gc_maybe_untrack(op)) {
            gc_restore_refs(op);
            return true;
        }
    }

    // We repurpose ob_tid to compute "gc_refs", the number of external
    // references to the object (i.e., from outside the GC heaps). This means
    // that ob_tid is no longer a valid thread id until it is restored by
    // scan_heap_visitor(). Until then, we cannot use the standard reference
    // counting functions or allow other threads to run Python code.
    gc_maybe_init_refs(op);

    // Add the actual refcount to ob_tid.
    gc_add_refs(op, refcount);

    // Subtract internal references from ob_tid. Objects with ob_tid > 0
    // are directly reachable from outside containers, and so can't be
    // collected.
    Py_TYPE(op)->tp_traverse(op, visit_decref, NULL);
    return true;
}

static int
visit_clear_unreachable(PyObject *op, void *stack)
{
    if (gc_is_unreachable(op)) {
        _PyObject_ASSERT(op, _PyObject_GC_IS_TRACKED(op));
        gc_clear_unreachable(op);
        return _PyObjectStack_Push((_PyObjectStack *)stack, op);
    }
    return 0;
}

// Transitively clear the unreachable bit on all objects reachable from op.
static int
mark_reachable(PyObject *op)
{
    _PyObjectStack stack = { NULL };
    do {
        traverseproc traverse = Py_TYPE(op)->tp_traverse;
        if (traverse(op, visit_clear_unreachable, &stack) < 0) {
            _PyObjectStack_Clear(&stack);
            return -1;
        }
        op = _PyObjectStack_Pop(&stack);
    } while (op != NULL);
    return 0;
}

#ifdef GC_DEBUG
static bool
validate_alive_bits(const mi_heap_t *heap, const mi_heap_area_t *area,
                   void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }

    _PyObject_ASSERT_WITH_MSG(op, !gc_is_alive(op),
                              "object should not be marked as alive yet");

    return true;
}

static bool
validate_refcounts(const mi_heap_t *heap, const mi_heap_area_t *area,
                   void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }

    // This assert mirrors the one in Python/gc.c:update_refs(). There must be
    // no tracked objects with a reference count of 0 when the cyclic
    // collector starts. If there is, then the collector will double dealloc
    // the object. The likely cause for hitting this is a faulty .tp_dealloc.
    // Also see the comment in `update_refs()`.
    _PyObject_ASSERT_WITH_MSG(op, Py_REFCNT(op) > 0,
                              "tracked objects must have a reference count > 0");

    _PyObject_ASSERT_WITH_MSG(op, !gc_is_unreachable(op),
                              "object should not be marked as unreachable yet");

    if (_Py_REF_IS_MERGED(op->ob_ref_shared)) {
        _PyObject_ASSERT_WITH_MSG(op, op->ob_tid == 0,
                                  "merged objects should have ob_tid == 0");
    }
    else if (!_Py_IsImmortal(op)) {
        _PyObject_ASSERT_WITH_MSG(op, op->ob_tid != 0,
                                  "unmerged objects should have ob_tid != 0");
    }

    return true;
}

static bool
validate_gc_objects(const mi_heap_t *heap, const mi_heap_area_t *area,
                    void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }

    if (gc_is_alive(op)) {
        _PyObject_ASSERT(op, !gc_is_unreachable(op));
        return true;
    }

    _PyObject_ASSERT(op, gc_is_unreachable(op));
    _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(op) >= 0,
                                  "refcount is too small");
    return true;
}
#endif

static bool
mark_object(PyObject *op, struct collection_state *state);

static bool
mark_heap_visitor(const mi_heap_t *heap, const mi_heap_area_t *area,
                  void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }
    return mark_object(op, (struct collection_state *)args);
}

static bool
mark_object(PyObject *op, struct collection_state *state)
{
    if (gc_is_alive(op) || !gc_is_unreachable(op)) {
        // Object was already marked as reachable.
        return true;
    }

    _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(op) >= 0,
                                  "refcount is too small");

    // GH-129236: If we've seen an active frame without a valid stack pointer,
    // then we can't collect objects with deferred references because we may
    // have missed some reference to the object on the stack. In that case,
    // treat the object as reachable even if gc_refs is zero.
    int keep_alive = (state->skip_deferred_objects &&
                      _PyObject_HasDeferredRefcount(op));

    if (gc_get_refs(op) != 0 || keep_alive) {
        // Object is reachable but currently marked as unreachable.
        // Mark it as reachable and traverse its pointers to find
        // any other object that may be directly reachable from it.
        gc_clear_unreachable(op);

        // Transitively mark reachable objects by clearing the unreachable flag.
        if (mark_reachable(op) < 0) {
            return false;
        }
    }

    return true;
}

static bool
restore_refs(const mi_heap_t *heap, const mi_heap_area_t *area,
             void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }
    gc_restore_tid(op);
    gc_clear_unreachable(op);
    gc_clear_alive(op);
    return true;
}

/* Return true if object has a pre-PEP 442 finalization method. */
static int
has_legacy_finalizer(PyObject *op)
{
    return Py_TYPE(op)->tp_del != NULL;
}

static void
scan_object(PyObject *op, struct collection_state *state);

static bool
scan_heap_visitor(const mi_heap_t *heap, const mi_heap_area_t *area,
                  void *block, size_t block_size, void *args)
{
    struct collection_state *state = (struct collection_state *)args;
    if (block == NULL) {
        // mimalloc visits each page's area before its blocks: a new page.
        if (gc_generational_enabled()) {
            gc_scan_page_done(state);
            state->scan_page = _mi_ptr_page(area->blocks);
            state->scan_page_used = (Py_ssize_t)area->used;
            state->scan_page_garbage = 0;
        }
        return true;
    }
    PyObject *op = op_from_block_all_gc(block, args);
    if (op == NULL) {
        return true;
    }
    // The free-threaded GC cost is proportional to the number of objects in
    // the mimalloc GC heap and so we should include the counts for untracked
    // and frozen objects as well.  This is especially important if many
    // tuples have been untracked.
    state->long_lived_total++;
    if (!_PyObject_GC_IS_TRACKED(op) || gc_is_frozen(op)) {
        return true;
    }
    scan_object(op, state);
    return true;
}

// Sort a tracked object after marking: garbage is queued, a reachable object
// gets its `ob_tid` back.
static void
scan_object(PyObject *op, struct collection_state *state)
{
    if (gc_is_unreachable(op)) {
        // (Garbage found by a full collection: its deallocation is not an
        // old object dying on its own.)
        gc_clear_bit(op, _PyGC_BITS_OLD);
        PyTypeObject *tp = Py_TYPE(op);
        if (tp->tp_finalize != NULL && !_PyGC_FINALIZED(op)) {
            state->has_finalizers = true;
        }
        if (PyWeakref_Check(op) ||
            (_PyType_SUPPORTS_WEAKREFS(tp) &&
             *_PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(op) != NULL))
        {
            state->has_weakrefs = true;
        }

        // Disable deferred refcounting for unreachable objects so that they
        // are collected immediately after finalization.
        disable_deferred_refcounting(op);

        // Merge and add one to the refcount to prevent deallocation while we
        // are holding on to it in a worklist.
        merge_refcount(op, 1);

        if (has_legacy_finalizer(op)) {
            // would be unreachable, but has legacy finalizer
            gc_clear_unreachable(op);
            worklist_push(&state->legacy_finalizers, op);
        }
        else {
            worklist_push(&state->unreachable, op);
            if (state->scan_page != NULL && _mi_ptr_page(op) == state->scan_page) {
                state->scan_page_garbage++;
            }
            gc_obj_array_push(&state->garbage, op);
        }
        // It is possible this object will be resurrected but
        // for now we assume it will be deallocated.
        state->long_lived_total--;
        return;
    }

    if (state->reason == _Py_GC_REASON_SHUTDOWN) {
        // Disable deferred refcounting for reachable objects as well during
        // interpreter shutdown. This ensures that these objects are collected
        // immediately when their last reference is removed.
        disable_deferred_refcounting(op);
    }

    // object is reachable, restore `ob_tid`; we're done with these objects
    gc_restore_tid(op);
    gc_clear_alive(op);
    if (!gc_has_bit(op, _PyGC_BITS_OLD) && gc_generational_enabled() &&
        !_PyObject_HasDeferredRefcount(op))
    {
        // A survivor: later young collections treat it as old, wherever it
        // lives (see "Object ages" below).
        gc_set_bit(op, _PyGC_BITS_OLD);
        state->aged++;
    }
}

static int
move_legacy_finalizer_reachable(struct collection_state *state);

#ifdef GC_ENABLE_MARK_ALIVE

static void
gc_prime_from_spans(gc_mark_args_t *args)
{
    unsigned int space = BUFFER_HI - gc_mark_buffer_len(args);
    // there should always be at least this amount of space
    assert(space <= gc_mark_buffer_avail(args));
    assert(space <= BUFFER_HI);
    gc_span_t entry = args->spans.stack[--args->spans.size];
    // spans on the stack should always have one or more elements
    assert(entry.start < entry.end);
    do {
        PyObject *op = *entry.start;
        entry.start++;
        if (op != NULL) {
            gc_mark_buffer_push(op, args);
            space--;
            if (space == 0) {
                // buffer is as full as we want and not done with span
                gc_mark_span_push(&args->spans, entry.start, entry.end);
                return;
            }
        }
    } while (entry.start < entry.end);
}

static void
gc_prime_buffer(gc_mark_args_t *args)
{
    if (args->spans.size > 0) {
        gc_prime_from_spans(args);
    }
    else {
        // When priming, don't fill the buffer too full since that would
        // likely cause the stack to be used shortly after when it
        // fills. We want to use the buffer as much as possible and so
        // we only fill to BUFFER_HI, not BUFFER_SIZE.
        Py_ssize_t space = BUFFER_HI - gc_mark_buffer_len(args);
        assert(space > 0);
        do {
            PyObject *op = _PyObjectStack_Pop(&args->stack);
            if (op == NULL) {
                return;
            }
            gc_mark_buffer_push(op, args);
            space--;
        } while (space > 0);
    }
}

static int
gc_propagate_alive_prefetch(gc_mark_args_t *args)
{
    for (;;) {
        Py_ssize_t buf_used = gc_mark_buffer_len(args);
        if (buf_used <= BUFFER_LO) {
            // The mark buffer is getting empty.  If it's too empty
            // then there will not be enough delay between issuing
            // the prefetch and when the object is actually accessed.
            // Prime the buffer with object pointers from the stack or
            // from the spans, if there are any available.
            gc_prime_buffer(args);
            if (gc_mark_buffer_is_empty(args)) {
                return 0;
            }
        }
        PyObject *op = gc_mark_buffer_pop(args);

        if (!gc_has_bit(op, _PyGC_BITS_TRACKED)) {
            continue;
        }

        if (gc_is_alive(op)) {
            continue; // already visited this object
        }

        // Need to call tp_traverse on this object. Mark it alive so we
        // don't traverse it a second time.
        gc_set_alive(op);

        traverseproc traverse = Py_TYPE(op)->tp_traverse;
        if (traverse == PyList_Type.tp_traverse) {
            if (gc_mark_traverse_list(op, args) < 0) {
                return -1;
            }
        }
        else if (traverse == PyTuple_Type.tp_traverse) {
            if (gc_mark_traverse_tuple(op, args) < 0) {
                return -1;
            }
        }
        else if (traverse(op, gc_mark_enqueue_buffer_visitproc, args) < 0) {
            return -1;
        }
    }
}

static int
gc_propagate_alive(gc_mark_args_t *args)
{
    if (args->use_prefetch) {
        return gc_propagate_alive_prefetch(args);
    }
    else {
        for (;;) {
            PyObject *op = _PyObjectStack_Pop(&args->stack);
            if (op == NULL) {
                break;
            }
            assert(_PyObject_GC_IS_TRACKED(op));
            assert(gc_is_alive(op));
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            if (traverse(op, gc_mark_enqueue_no_buffer_visitproc, args) < 0) {
                return -1;
            }
        }
        return 0;
    }
}

// Using tp_traverse, mark everything reachable from known root objects
// (which must be non-garbage) as alive (_PyGC_BITS_ALIVE is set).  In
// most programs, this marks nearly all objects that are not actually
// unreachable.
//
// Actually alive objects can be missed in this pass if they are alive
// due to being referenced from an unknown root (e.g. an extension
// module global), some tp_traverse methods are either missing or not
// accurate, or objects that have been untracked.  Objects that are only
// reachable from the aforementioned are also missed.
//
// If gc.freeze() is used, this pass is disabled since it is unlikely to
// help much.  The next stages of cyclic GC will ignore objects with the
// alive bit set.
//
// Returns -1 on failure (out of memory).
static int
gc_mark_alive_from_roots(PyInterpreterState *interp,
                         struct collection_state *state)
{
#ifdef GC_DEBUG
    // Check that all objects don't have alive bit set
    gc_visit_heaps(interp, &validate_alive_bits, &state->base);
#endif
    gc_mark_args_t mark_args = { 0 };

    // Using prefetch instructions is only a win if the set of objects being
    // examined by the GC does not fit into CPU caches.  Otherwise, using the
    // buffer and prefetch instructions is just overhead.  Using the long lived
    // object count seems a good estimate of if things will fit in the cache.
    // On 64-bit platforms, the minimum object size is 32 bytes.  A 4MB L2 cache
    // would hold about 130k objects.
    mark_args.use_prefetch = interp->gc.long_lived_total > 200000;

    #define MARK_ENQUEUE(op) \
        if (op != NULL ) { \
            if (gc_mark_enqueue(op, &mark_args) < 0) { \
                gc_abort_mark_alive(interp, state, &mark_args); \
                return -1; \
            } \
        }
    MARK_ENQUEUE(interp->sysdict);
#ifdef GC_MARK_ALIVE_EXTRA_ROOTS
    MARK_ENQUEUE(interp->builtins);
    MARK_ENQUEUE(interp->dict);
    struct types_state *types = &interp->types;
    for (int i = 0; i < _Py_MAX_MANAGED_STATIC_BUILTIN_TYPES; i++) {
        MARK_ENQUEUE(types->builtins.initialized[i].tp_dict);
        MARK_ENQUEUE(types->builtins.initialized[i].tp_subclasses);
    }
    for (int i = 0; i < _Py_MAX_MANAGED_STATIC_EXT_TYPES; i++) {
        MARK_ENQUEUE(types->for_extensions.initialized[i].tp_dict);
        MARK_ENQUEUE(types->for_extensions.initialized[i].tp_subclasses);
    }
#endif
#ifdef GC_MARK_ALIVE_STACKS
    if (gc_visit_thread_stacks_mark_alive(interp, &mark_args) < 0) {
        gc_abort_mark_alive(interp, state, &mark_args);
        return -1;
    }
#endif
    #undef MARK_ENQUEUE

    // Use tp_traverse to find everything reachable from roots.
    if (gc_propagate_alive(&mark_args) < 0) {
        gc_abort_mark_alive(interp, state, &mark_args);
        return -1;
    }

    assert(mark_args.spans.size == 0);
    if (mark_args.spans.stack != NULL) {
        PyMem_Free(mark_args.spans.stack);
    }
    assert(mark_args.stack.head == NULL);

    return 0;
}
#endif // GC_ENABLE_MARK_ALIVE


// Generational collection (experimental, PYTHON_GC_GENERATIONAL=1).
//
// GC objects are allocated in the "young" mimalloc heaps.  A young
// collection considers only the objects on pages of those heaps: the usual
// algorithm (refcount minus references from other candidates) treats any
// reference from outside the candidate set -- including from old objects --
// as external, so no write barrier is needed.  Garbage cycles that include
// old objects are left for full collections.  The scan of each collection
// counts, per page, the blocks in use and the garbage among them; young pages
// whose survivors fill at least GC_PROMOTE_PERCENT of them are moved to the
// old heaps once the scan is done.  Sparser pages stay young (and keep being
// allocated from), so promotion doesn't strand free space.  Any subset of
// objects is a valid candidate set, so objects that stay young are merely
// examined again.

// Page occupancy thresholds, in percent of the page's capacity.
#define GC_PROMOTE_PERCENT 80
#define GC_DEMOTE_PERCENT 50

// Young collections run every GC_YOUNG_THRESHOLD new GC objects (or sooner,
// when the full collection threshold is lower).  The trigger adapts: when
// at least GC_YOUNG_SURVIVAL_HIGH percent of a young collection's candidates
// survive, collecting that often finds little, and the trigger doubles (up to
// GC_YOUNG_THRESHOLD_MAX); below GC_YOUNG_SURVIVAL_LOW percent it halves.
#define GC_YOUNG_THRESHOLD 20000
#define GC_YOUNG_THRESHOLD_MAX (8 * GC_YOUNG_THRESHOLD)
#define GC_YOUNG_SURVIVAL_HIGH 60
#define GC_YOUNG_SURVIVAL_LOW 40

static int
gc_generational_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("PYTHON_GC_GENERATIONAL");
        enabled = env != NULL && atoi(env) > 0;
    }
    return enabled;
}

static bool
gc_is_young_page(mi_page_t *page)
{
    return (page->tag == _Py_MIMALLOC_HEAP_GC_YOUNG ||
            page->tag == _Py_MIMALLOC_HEAP_GC_PRE_YOUNG);
}

static bool
gc_is_young_object(PyObject *op)
{
    return (!gc_has_bit(op, _PyGC_BITS_OLD) &&
            gc_is_young_page(_mi_ptr_page(op)));
}

static bool
gc_page_list_push(struct gc_page_list *l, mi_page_t *page)
{
    if (l->n == l->cap) {
        Py_ssize_t cap = l->cap ? l->cap * 2 : 256;
        mi_page_t **pages = PyMem_RawRealloc(l->pages, cap * sizeof(*pages));
        if (pages == NULL) {
            return false;   // out of memory: the page just doesn't move
        }
        l->pages = pages;
        l->cap = cap;
    }
    l->pages[l->n++] = page;
    return true;
}

static void
gc_page_list_clear(struct gc_page_list *l)
{
    PyMem_RawFree(l->pages);
    l->pages = NULL;
    l->n = l->cap = 0;
}

// Called by the scan when it is done with a page: decide from the page's
// surviving blocks whether it moves.  A young page is promoted if its
// survivors fill at least GC_PROMOTE_PERCENT of it and it already survived a
// previous collection (a tenuring threshold, so that objects merely in use
// at the time of one collection, e.g. in other threads, aren't promoted).
//
// Demotion: an old page whose surviving blocks fall below GC_DEMOTE_PERCENT
// of its capacity moves back to the young heap, so that its free blocks are
// allocated from again instead of being stranded until the page is
// completely empty.  Its remaining objects become young candidates again
// (any subset of objects is a valid candidate set).  The gap to
// GC_PROMOTE_PERCENT keeps pages from bouncing.  Only full collections
// demote, from the scan like promotion: a collection only touches the pages
// it examines, so young collections stay independent of the old heap's size.
static void
gc_scan_page_done(struct collection_state *state)
{
    mi_page_t *page = state->scan_page;
    state->scan_page = NULL;
    if (page == NULL || mi_page_heap(page) == NULL) {
        return;     // (pages of abandoned segments stay where they are)
    }
    Py_ssize_t live = state->scan_page_used - state->scan_page_garbage;
    if (live <= 0) {
        return;
    }
    size_t capacity = page->capacity;
    if (gc_is_young_page(page)) {
        if ((size_t)live * 100 < (size_t)GC_PROMOTE_PERCENT * capacity) {
            return;
        }
        if (!page->gc_survived) {
            page->gc_survived = 1;
            return;
        }
        if (gc_page_list_push(&state->promote, page)) {
            state->promote_live += live;
        }
    }
    else if ((size_t)live * 100 < (size_t)GC_DEMOTE_PERCENT * capacity) {
        page->gc_survived = 0;
        if (gc_page_list_push(&state->demote, page)) {
            state->demote_live += live;
        }
    }
}

// Move the pages the scan decided on.  Returns the net number of live
// objects moved to the old heaps.  Called with the world stopped, after the
// scan.
static Py_ssize_t
gc_move_pages(struct collection_state *state)
{
    // A thread's young and old heaps are adjacent entries of its heap array.
    const ptrdiff_t to_old = _Py_MIMALLOC_HEAP_GC - _Py_MIMALLOC_HEAP_GC_YOUNG;
    static_assert(_Py_MIMALLOC_HEAP_GC_PRE - _Py_MIMALLOC_HEAP_GC_PRE_YOUNG ==
                  _Py_MIMALLOC_HEAP_GC - _Py_MIMALLOC_HEAP_GC_YOUNG,
                  "young and old heaps must be the same distance apart");
    for (Py_ssize_t i = 0; i < state->promote.n; i++) {
        mi_page_t *page = state->promote.pages[i];
        _mi_page_move_to_heap(page, mi_page_heap(page) + to_old);
    }
    for (Py_ssize_t i = 0; i < state->demote.n; i++) {
        mi_page_t *page = state->demote.pages[i];
        _mi_page_move_to_heap(page, mi_page_heap(page) - to_old);
    }
    Py_ssize_t moved = state->promote_live - state->demote_live;
    gc_page_list_clear(&state->promote);
    gc_page_list_clear(&state->demote);
    state->promote_live = state->demote_live = 0;
    return moved;
}


// Young collections.  update_refs() initializes the refs of the objects it
// reaches through tp_traverse, which would overwrite ob_tid of old objects;
// a young collection instead initializes all candidates first, then
// subtracts the references between candidates.  Marking and scanning only
// act on candidates (objects with the UNREACHABLE bit).
//
// Object ages: an object that survives a collection gets _PyGC_BITS_OLD, and
// is not a candidate of later young collections even while its page is still
// young (pages are only promoted when they are dense, so that promotion
// doesn't strand free space; sparse pages would otherwise make every young
// collection re-examine, and traverse, their survivors).  Old objects on
// young pages are like objects on old pages: references from them keep
// candidates alive, and garbage cycles through them wait for a full
// collection.  New objects, including ones in reused memory, start young.
//
// Objects with deferred reference counts (top-level functions, methods
// defined in class bodies, modules, types and descriptors) are not
// candidates: they are treated as alive, like old objects.  References from
// thread stacks only need to be counted for those objects (other stack
// references are counted in the refcount, or borrowed from a counted
// reference in the same frame), so young collections don't walk the thread
// stacks, and their cost doesn't depend on stack depth.

// Called by the young walk when it is done with a page: a young page whose
// old objects (survivors of earlier collections) fill at least
// GC_PROMOTE_PERCENT of it moves to the old heap, so that later young
// collections don't walk it.
static void
gc_young_page_done(struct collection_state *state)
{
    mi_page_t *page = state->scan_page;
    state->scan_page = NULL;
    if (page == NULL || mi_page_heap(page) == NULL) {
        return;     // (pages of abandoned segments stay where they are)
    }
    Py_ssize_t old = state->scan_page_old;
    if ((size_t)old * 100 >= (size_t)GC_PROMOTE_PERCENT * page->capacity) {
        if (gc_page_list_push(&state->promote, page)) {
            state->promote_live += old;
        }
    }
}

static bool
young_init_refs(const mi_heap_t *heap, const mi_heap_area_t *area,
                void *block, size_t block_size, void *args)
{
    struct collection_state *state = (struct collection_state *)args;
    if (block == NULL) {
        // mimalloc visits each page's area before its blocks: a new page.
        gc_young_page_done(state);
        state->scan_page = _mi_ptr_page(area->blocks);
        state->scan_page_old = 0;
        return true;
    }
    PyObject *op = op_from_block(block, args, false);
    if (op == NULL) {
        return true;
    }
    if (gc_has_bit(op, _PyGC_BITS_OLD)) {
        state->scan_page_old++;
        return true;
    }
    if (_PyObject_HasDeferredRefcount(op)) {
        return true;
    }
    if (_Py_IsImmortal(op)) {
        op->ob_tid = 0;
        _PyObject_GC_UNTRACK(op);
        gc_clear_unreachable(op);
        return true;
    }
    Py_ssize_t refcount = Py_REFCNT(op);
    _PyObject_ASSERT(op, refcount >= 0);
    if (refcount > 0 && gc_maybe_untrack(op)) {
        return true;
    }
    if (state->young_n == state->young_cap) {
        Py_ssize_t cap = state->young_cap ? state->young_cap * 2 : 4096;
        PyObject **objs = PyMem_RawRealloc(state->young_objs, cap * sizeof(*objs));
        if (objs == NULL) {
            state->young_oom = true;
            return true;
        }
        state->young_objs = objs;
        state->young_cap = cap;
    }
    state->young_objs[state->young_n++] = op;
    state->candidates++;
    assert(!gc_is_alive(op) && !gc_is_unreachable(op));
    gc_set_unreachable(op);
    op->ob_tid = (uintptr_t)refcount;
    return true;
}

static int
visit_decref_young(PyObject *op, void *arg)
{
    if (gc_is_unreachable(op)) {
        gc_decref(op);
    }
    return 0;
}

static int
gc_visit_young_heaps(PyInterpreterState *interp, mi_block_visit_fun *visitor,
                     struct visitor_args *arg)
{
    assert(interp->stoptheworld.world_stopped);
    HEAD_LOCK(&_PyRuntime);
    int err = gc_visit_heaps_lock_held(interp, visitor, arg, true);
    HEAD_UNLOCK(&_PyRuntime);
    return err;
}

static void
young_restore_refs(struct collection_state *state)
{
    for (Py_ssize_t i = 0; i < state->young_n; i++) {
        PyObject *op = state->young_objs[i];
        gc_restore_tid(op);
        gc_clear_unreachable(op);
        gc_clear_alive(op);
    }
}

static int
deduce_unreachable_young(PyInterpreterState *interp,
                         struct collection_state *state)
{
    // One walk of the young pages finds the candidates; the other phases go
    // over the candidates only, so their cost doesn't depend on how many old
    // objects the young pages hold.
    gc_visit_young_heaps(interp, &young_init_refs, &state->base);
    gc_young_page_done(state);
    PyObject **objs = state->young_objs;
    Py_ssize_t n = state->young_n;
    int err = 0;
    if (state->young_oom) {
        young_restore_refs(state);
        err = -1;
    }
    else {
        for (Py_ssize_t i = 0; i < n; i++) {
            Py_TYPE(objs[i])->tp_traverse(objs[i], visit_decref_young, NULL);
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            if (!mark_object(objs[i], state)) {
                // On out-of-memory, restore the refcounts and bail out.
                young_restore_refs(state);
                err = -1;
                break;
            }
        }
    }
    if (err == 0) {
        for (Py_ssize_t i = 0; i < n; i++) {
            scan_object(objs[i], state);
        }
    }
    PyMem_RawFree(state->young_objs);
    state->young_objs = NULL;
    state->young_n = state->young_cap = 0;
    if (err < 0) {
        return -1;
    }

    if (state->legacy_finalizers.head) {
        if (move_legacy_finalizer_reachable(state) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
deduce_unreachable_heap(PyInterpreterState *interp,
                        struct collection_state *state)
{
    if (state->young) {
        return deduce_unreachable_young(interp, state);
    }
    // Identify objects that are directly reachable from outside the GC heap
    // by computing the difference between the refcount and the number of
    // incoming references.
    gc_visit_heaps(interp, &update_refs, &state->base);
    GC_PHASE_DONE(state, GC_PHASE_UPDATE_REFS);

#ifdef GC_DEBUG
    // Check that all objects are marked as unreachable and that the computed
    // reference count difference (stored in `ob_tid`) is non-negative.
    gc_visit_heaps(interp, &validate_gc_objects, &state->base);
#endif

    // Visit the thread stacks to account for any deferred references.
    gc_visit_thread_stacks(interp, state);

    // Transitively mark reachable objects by clearing the
    // _PyGC_BITS_UNREACHABLE flag.
    if (gc_visit_heaps(interp, &mark_heap_visitor, &state->base) < 0) {
        // On out-of-memory, restore the refcounts and bail out.
        gc_visit_heaps(interp, &restore_refs, &state->base);
        return -1;
    }
    GC_PHASE_DONE(state, GC_PHASE_MARK_HEAP);

    // Identify remaining unreachable objects and push them onto a stack.
    // Restores ob_tid for reachable objects.
    gc_visit_heaps(interp, &scan_heap_visitor, &state->base);
    gc_scan_page_done(state);
    GC_PHASE_DONE(state, GC_PHASE_SCAN_HEAP);

    if (state->legacy_finalizers.head) {
        // There may be objects reachable from legacy finalizers that are in
        // the unreachable set. We need to mark them as reachable.
        if (move_legacy_finalizer_reachable(state) < 0) {
            return -1;
        }
    }

    return 0;
}

static int
move_legacy_finalizer_reachable(struct collection_state *state)
{
    // Clear the reachable bit on all objects transitively reachable
    // from the objects with legacy finalizers.
    PyObject *op;
    WORKSTACK_FOR_EACH(&state->legacy_finalizers, op) {
        if (mark_reachable(op) < 0) {
            return -1;
        }
    }

    // Move the reachable objects from the unreachable worklist to the legacy
    // finalizer worklist.
    struct worklist_iter iter;
    WORKSTACK_FOR_EACH_ITER(&state->unreachable, &iter, op) {
        if (!gc_is_unreachable(op)) {
            worklist_remove(&iter);
            worklist_push(&state->legacy_finalizers, op);
        }
    }

    return 0;
}

// Weakrefs with callbacks are enqueued in `wrcb_to_call`, but not invoked
// yet.  Note that it's possible for such weakrefs to be outside the
// unreachable set -- indeed, those are precisely the weakrefs whose callbacks
// must be invoked.  See gc_weakref.txt for overview & some details.
//
// The clearing of weakrefs is suble and must be done carefully, as there was
// previous bugs related to this.  First, weakrefs to the unreachable set of
// objects must be cleared before we start calling `tp_clear`.  If we don't,
// those weakrefs can reveal unreachable objects to Python-level code and that
// is not safe.  Many objects are not usable after `tp_clear` has been called
// and could even cause crashes if accessed (see bpo-38006 and gh-91636 as
// example bugs).
//
// Weakrefs with callbacks always need to be cleared before executing the
// callback.  That's because sometimes a callback will call the ref object,
// to check if the reference is actually dead (KeyedRef does this, for
// example).  We want to indicate that it is dead, even though it is possible
// a finalizer might resurrect it.  Clearing also prevents the callback from
// executing more than once.
//
// Since Python 2.3, all weakrefs to cyclic garbage have been cleared *before*
// calling finalizers.  However, since tp_subclasses started being necessary
// to invalidate caches (e.g. by PyType_Modified), that clearing has created
// a bug.  If the weakref to the subclass is cleared before a finalizer is
// called, the cache may not be correctly invalidated.  That can lead to
// segfaults since the caches can refer to deallocated objects (GH-91636
// is an example).  Now, we delay the clear of weakrefs without callbacks
// until *after* finalizers have been executed.  That means weakrefs without
// callbacks are still usable while finalizers are being executed.
//
// The weakrefs that are inside the unreachable set must also be cleared.
// The reason this is required is not immediately obvious.  If the weakref
// refers to an object outside of the unreachable set, that object might go
// away when we start clearing objects.  Normally, the object should also be
// part of the unreachable set but that's not true in the case of incomplete
// or missing `tp_traverse` methods.  When that object goes away, the callback
// for weakref can be executed and that could reveal unreachable objects to
// Python-level code.  See bpo-38006 as an example bug.
static void
find_weakref_callbacks(struct collection_state *state)
{
    PyObject *op;
    WORKSTACK_FOR_EACH(&state->unreachable, op) {
        if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(op))) {
            continue;
        }

        // NOTE: This is never triggered for static types so we can avoid the
        // (slightly) more costly _PyObject_GET_WEAKREFS_LISTPTR().
        PyWeakReference **wrlist = _PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(op);

        // `op` may have some weakrefs.  March over the list, clear
        // all the weakrefs, and enqueue the weakrefs with callbacks
        // that must be called into wrcb_to_call.
        PyWeakReference *next_wr;
        for (PyWeakReference *wr = *wrlist; wr != NULL; wr = next_wr) {
            // Get the next list element to get iterator progress if we omit
            // clearing of the weakref (because _PyWeakref_ClearRef changes
            // next pointer in the wrlist).
            next_wr = wr->wr_next;

            // Weakrefs with callbacks always need to be cleared before
            // executing the callback.
            if (wr->wr_callback != NULL) {
                // _PyWeakref_ClearRef clears the weakref but leaves the
                // callback pointer intact.  Obscure: it also changes *wrlist.
                _PyObject_ASSERT((PyObject *)wr, wr->wr_object == op);
                _PyWeakref_ClearRef(wr);
                _PyObject_ASSERT((PyObject *)wr, wr->wr_object == Py_None);
            }

            // We do not invoke callbacks for weakrefs that are themselves
            // unreachable. This is partly for historical reasons: weakrefs
            // predate safe object finalization, and a weakref that is itself
            // unreachable may have a callback that resurrects other
            // unreachable objects.
            if (wr->wr_callback == NULL || gc_is_unreachable((PyObject *)wr)) {
                continue;
            }

            // Create a new reference so that wr can't go away before we can
            // process it again.
            merge_refcount((PyObject *)wr, 1);

            // Enqueue weakref to be called later.
            worklist_push(&state->wrcb_to_call, (PyObject *)wr);
        }
    }
}

// Clear weakrefs to objects in the unreachable set.  See comments
// above find_weakref_callbacks() for why this clearing is required.
static void
clear_weakrefs(struct collection_state *state)
{
    PyObject *op;
    WORKSTACK_FOR_EACH(&state->unreachable, op) {
        if (PyWeakref_Check(op)) {
            // Clear weakrefs that are themselves unreachable.
            _PyWeakref_ClearRef((PyWeakReference *)op);
        }

        if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(op))) {
            continue;
        }

        // NOTE: This is never triggered for static types so we can avoid the
        // (slightly) more costly _PyObject_GET_WEAKREFS_LISTPTR().
        PyWeakReference **wrlist = _PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(op);

        // `op` may have some weakrefs.  March over the list, clear
        // all the weakrefs.
        for (PyWeakReference *wr = *wrlist; wr != NULL; wr = *wrlist) {
            // _PyWeakref_ClearRef clears the weakref but leaves
            // the callback pointer intact.  Obscure: it also
            // changes *wrlist.
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == op);
            _PyWeakref_ClearRef(wr);
            _PyObject_ASSERT((PyObject *)wr, wr->wr_object == Py_None);
        }
    }
}

static void
call_weakref_callbacks(struct collection_state *state)
{
    // Invoke the callbacks we decided to honor.
    PyObject *op;
    while ((op = worklist_pop(&state->wrcb_to_call)) != NULL) {
        _PyObject_ASSERT(op, PyWeakref_Check(op));

        PyWeakReference *wr = (PyWeakReference *)op;
        PyObject *callback = wr->wr_callback;
        _PyObject_ASSERT(op, callback != NULL);

        /* copy-paste of weakrefobject.c's handle_callback() */
        PyObject *temp = PyObject_CallOneArg(callback, (PyObject *)wr);
        if (temp == NULL) {
            PyErr_FormatUnraisable("Exception ignored while "
                                   "calling weakref callback %R", callback);
        }
        else {
            Py_DECREF(temp);
        }

        Py_DECREF(op);  // drop worklist reference
    }
}


static GCState *
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}


// The byte trigger.  Collections are triggered by the number of new GC
// objects, which says nothing about the memory they hold: a few objects
// with large buffers (or lists, dict tables, strings) can keep gigabytes
// alive as garbage.  So a collection also runs once the memory on the side
// (see _PyGC_HeapBytesChanged()) grew by GC_SIDE_BYTES_PERCENT of what was
// in use before, and by at least GC_SIDE_BYTES_MIN.  The floor matters: a
// batch of garbage that fits in the CPU cache is freed and reused while it
// is still there.
//
// Only with generational collection: the trigger makes collections more
// frequent, which pays when they are young collections and costs more than
// it saves when every one of them scans the whole heap.
#define GC_SIDE_BYTES_PERCENT 25
#define GC_SIDE_BYTES_MIN ((Py_ssize_t)16 << 20)

void
_PyGC_InitState(GCState *gcstate)
{
    // TODO: move to pycore_runtime_init.h once the incremental GC lands.
    gcstate->young.threshold = 2000;
}


PyStatus
_PyGC_Init(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;

    gcstate->generation_stats = PyMem_RawCalloc(1, sizeof(struct gc_stats));
    if (gcstate->generation_stats == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    gcstate->garbage = PyList_New(0);
    if (gcstate->garbage == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    gcstate->callbacks = PyList_New(0);
    if (gcstate->callbacks == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}

static void
debug_cycle(const char *msg, PyObject *op)
{
    PySys_FormatStderr("gc: %s <%s %p>\n",
                       msg, Py_TYPE(op)->tp_name, op);
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
static void
finalize_garbage(struct collection_state *state)
{
    // NOTE: the unreachable worklist holds a strong reference to the object
    // to prevent it from being deallocated while we are holding on to it.
    PyObject *op;
    WORKSTACK_FOR_EACH(&state->unreachable, op) {
        if (!_PyGC_FINALIZED(op)) {
            destructor finalize = Py_TYPE(op)->tp_finalize;
            if (finalize != NULL) {
                _PyGC_SET_FINALIZED(op);
                finalize(op);
                assert(!_PyErr_Occurred(_PyThreadState_GET()));
            }
        }
    }
}

// Garbage that other threads can help free.  While the collector is freeing
// a large amount of garbage, threads that allocate GC objects are asked (at
// their next eval breaker check) to free a chunk of it.  Without this, a
// single collecting thread can fall behind threads that create cyclic
// garbage, and the amount of garbage per collection grows without bound.
struct gc_delete_work {
    struct gc_obj_chunk *chunks;    // unclaimed chunks
    Py_ssize_t in_flight;           // claimed chunks not yet finished
    Py_ssize_t collected;
    PyEvent done;                   // set when in_flight drops to zero
};

// Clear and release the objects in a chunk of unreachable objects.  Returns
// the number of objects collected.
static Py_ssize_t
delete_garbage_chunk(PyThreadState *tstate, struct gc_obj_chunk *chunk)
{
    Py_ssize_t collected = 0;
    // Garbage often references objects owned by other threads (e.g. ints in
    // a list); batch the resulting biased refcount queueing.
    _Py_brc_begin_deferred(tstate);
    // The memory of most garbage belongs to other threads' heaps; batch
    // those frees too.
    _mi_free_batch_begin();
    for (Py_ssize_t i = 0; i < chunk->n; i++) {
        PyObject *op = chunk->items[i];
        if (!(_Py_atomic_load_uint8_relaxed(&op->ob_gc_bits) & _PyGC_BITS_UNREACHABLE)) {
            // Moved to the legacy finalizers list, which owns it.
            continue;
        }
        // Unlink from the unreachable worklist, if it is still linked.
        // Other threads freeing garbage may read ob_tid concurrently (in
        // Py_DECREF), as with worklist_pop().
        _Py_atomic_store_uintptr_relaxed(&op->ob_tid, 0);
        _PyObject_CLEAR_GC_BITS(op, _PyGC_BITS_UNREACHABLE);
        if (_PyObject_GC_IS_TRACKED(op)) {
            collected++;
            inquiry clear = Py_TYPE(op)->tp_clear;
            if (clear != NULL) {
                (void) clear(op);
                if (_PyErr_Occurred(tstate)) {
                    PyErr_FormatUnraisable("Exception ignored in tp_clear of %s",
                                           Py_TYPE(op)->tp_name);
                }
            }
        }
        Py_DECREF(op);  // drop the reference from the worklist
    }
    _mi_free_batch_end();
    _Py_brc_end_deferred(tstate);
    return collected;
}

// Claim and free one chunk of the current collection's garbage, if any.
static void
gc_assist_delete(PyThreadState *tstate)
{
    GCState *gcstate = &tstate->interp->gc;
    PyMutex_Lock(&gcstate->delete_mutex);
    struct gc_delete_work *work = gcstate->delete_work;
    struct gc_obj_chunk *chunk = NULL;
    if (work != NULL && work->chunks != NULL) {
        chunk = work->chunks;
        work->chunks = chunk->next;
        work->in_flight++;
    }
    PyMutex_Unlock(&gcstate->delete_mutex);
    if (chunk == NULL) {
        return;
    }

    Py_ssize_t collected = delete_garbage_chunk(tstate, chunk);
    PyMem_RawFree(chunk);

    PyMutex_Lock(&gcstate->delete_mutex);
    work->collected += collected;
    if (--work->in_flight == 0 && work->chunks == NULL) {
        _PyEvent_Notify(&work->done);
    }
    // The collector may free `work` as soon as the mutex is released.
    PyMutex_Unlock(&gcstate->delete_mutex);
}

// Free the unreachable objects with help from other threads.  Returns false
// if the objects could not be split into chunks, in which case the caller
// frees any objects left on the worklist.
static bool
delete_garbage_parallel(PyThreadState *tstate, struct collection_state *state)
{
    GCState *gcstate = state->gcstate;
    struct gc_delete_work work = {0};
    struct gc_obj_array arr = {0};
    PyObject *op;
    if (state->garbage.head != NULL && !state->garbage.failed) {
        // Use the array recorded during the heap scan rather than following
        // the worklist, which would leave other threads with no chunks to
        // help with until we reached its end.
        arr = state->garbage;
        state->garbage = (struct gc_obj_array){0};
        state->unreachable.head = 0;
    }
    gc_obj_array_clear(&state->garbage);
    while ((op = worklist_pop(&state->unreachable)) != NULL) {
        gc_obj_array_push(&arr, op);
        if (arr.failed) {
            // Out of memory: put it back and free everything serially.
            worklist_push(&state->unreachable, op);
            for (struct gc_obj_chunk *c = arr.head; c != NULL; c = c->next) {
                state->collected += delete_garbage_chunk(tstate, c);
            }
            gc_obj_array_clear(&arr);
            return false;
        }
    }
    if (arr.head == NULL) {
        return true;
    }
    if (arr.head->next == NULL) {
        // A single chunk isn't worth sharing.
        state->collected += delete_garbage_chunk(tstate, arr.head);
        gc_obj_array_clear(&arr);
        return true;
    }

    work.chunks = arr.head;
    PyMutex_Lock(&gcstate->delete_mutex);
    _Py_atomic_store_ptr_release(&gcstate->delete_work, &work);
    PyMutex_Unlock(&gcstate->delete_mutex);

    // Free chunks until there are none left to claim.
    for (;;) {
        PyMutex_Lock(&gcstate->delete_mutex);
        struct gc_obj_chunk *chunk = work.chunks;
        if (chunk == NULL) {
            _Py_atomic_store_ptr_release(&gcstate->delete_work, NULL);
            bool wait = work.in_flight > 0;
            PyMutex_Unlock(&gcstate->delete_mutex);
            if (wait) {
                // Wait for other threads to finish their chunks.
                PyEvent_Wait(&work.done);
            }
            break;
        }
        work.chunks = chunk->next;
        PyMutex_Unlock(&gcstate->delete_mutex);

        state->collected += delete_garbage_chunk(tstate, chunk);
        PyMem_RawFree(chunk);
    }

    // Synchronize with the last helper's update of `work`.
    PyMutex_Lock(&gcstate->delete_mutex);
    state->collected += work.collected;
    state->assisted = work.collected;
    PyMutex_Unlock(&gcstate->delete_mutex);
    return true;
}

// Break reference cycles by clearing the containers involved.
static void
delete_garbage(struct collection_state *state)
{
    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = state->gcstate;

    assert(!_PyErr_Occurred(tstate));

    PyObject *op;
    while ((op = worklist_pop(&state->objs_to_decref)) != NULL) {
        Py_DECREF(op);
    }

    if (!(gcstate->debug & _PyGC_DEBUG_SAVEALL) &&
        state->reason != _Py_GC_REASON_SHUTDOWN)
    {
        if (delete_garbage_parallel(tstate, state)) {
            return;
        }
    }

    while ((op = worklist_pop(&state->unreachable)) != NULL) {
        _PyObject_ASSERT(op, gc_is_unreachable(op));

        // Clear the unreachable flag.
        gc_clear_unreachable(op);

        if (!_PyObject_GC_IS_TRACKED(op)) {
            // Object might have been untracked by some other tp_clear() call.
            Py_DECREF(op);  // drop the reference from the worklist
            continue;
        }

        state->collected++;

        if (gcstate->debug & _PyGC_DEBUG_SAVEALL) {
            assert(gcstate->garbage != NULL);
            if (PyList_Append(gcstate->garbage, op) < 0) {
                _PyErr_Clear(tstate);
            }
        }
        else {
            inquiry clear = Py_TYPE(op)->tp_clear;
            if (clear != NULL) {
                (void) clear(op);
                if (_PyErr_Occurred(tstate)) {
                    PyErr_FormatUnraisable("Exception ignored in tp_clear of %s",
                                           Py_TYPE(op)->tp_name);
                }
            }
        }

        Py_DECREF(op);  // drop the reference from the worklist
    }
}

static void
handle_legacy_finalizers(struct collection_state *state)
{
    GCState *gcstate = state->gcstate;
    assert(gcstate->garbage != NULL);

    PyObject *op;
    while ((op = worklist_pop(&state->legacy_finalizers)) != NULL) {
        state->uncollectable++;

        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            debug_cycle("uncollectable", op);
        }

        if ((gcstate->debug & _PyGC_DEBUG_SAVEALL) || has_legacy_finalizer(op)) {
            if (PyList_Append(gcstate->garbage, op) < 0) {
                PyErr_Clear();
            }
        }
        Py_DECREF(op);  // drop worklist reference
    }
}

// Show stats for objects in each generations
static void
show_stats_each_generations(GCState *gcstate)
{
    // TODO
}

// Traversal callback for handle_resurrected_objects.
static int
visit_decref_unreachable(PyObject *op, void *data)
{
    if (gc_is_unreachable(op) && _PyObject_GC_IS_TRACKED(op)) {
        op->ob_ref_local -= 1;
    }
    return 0;
}

int
_PyGC_VisitStackRef(_PyStackRef *ref, visitproc visit, void *arg)
{
    // This is a bit tricky! We want to ignore deferred references when
    // computing the incoming references, but otherwise treat them like
    // regular references.
    assert(!PyStackRef_IsTaggedInt(*ref));
    if (PyStackRef_RefcountOnObject(*ref) ||
        (visit != visit_decref && visit != visit_decref_unreachable &&
         visit != visit_decref_young))
    {
        Py_VISIT(PyStackRef_AsPyObjectBorrow(*ref));
    }
    return 0;
}

int
_PyGC_VisitFrameStack(_PyInterpreterFrame *frame, visitproc visit, void *arg)
{
    _PyStackRef *ref = _PyFrame_GetLocalsArray(frame);
    /* locals and stack */
    for (; ref < frame->stackpointer; ref++) {
        _Py_VISIT_STACKREF(*ref);
    }
    return 0;
}

// Handle objects that may have resurrected after a call to 'finalize_garbage'.
static int
handle_resurrected_objects(struct collection_state *state)
{
    // First, find externally reachable objects by computing the reference
    // count difference in ob_ref_local. We can't use ob_tid here because
    // that's already used to store the unreachable worklist.
    PyObject *op;
    struct worklist_iter iter;
    WORKSTACK_FOR_EACH_ITER(&state->unreachable, &iter, op) {
        assert(gc_is_unreachable(op));
        assert(_Py_REF_IS_MERGED(op->ob_ref_shared));

        if (!_PyObject_GC_IS_TRACKED(op)) {
            // Object was untracked by a finalizer. Schedule it for a Py_DECREF
            // after we finish with the stop-the-world pause.
            gc_clear_unreachable(op);
            worklist_remove(&iter);
            worklist_push(&state->objs_to_decref, op);
            continue;
        }

        Py_ssize_t refcount = (op->ob_ref_shared >> _Py_REF_SHARED_SHIFT);
        if (refcount > INT32_MAX) {
            // The refcount is too big to fit in `ob_ref_local`. Mark the
            // object as immortal and bail out.
            gc_clear_unreachable(op);
            worklist_remove(&iter);
            _Py_SetImmortal(op);
            continue;
        }

        op->ob_ref_local += (uint32_t)refcount;

        // Subtract one to account for the reference from the worklist.
        op->ob_ref_local -= 1;

        traverseproc traverse = Py_TYPE(op)->tp_traverse;
        (void)traverse(op, visit_decref_unreachable, NULL);
    }

    // Find resurrected objects
    bool any_resurrected = false;
    WORKSTACK_FOR_EACH(&state->unreachable, op) {
        int32_t gc_refs = (int32_t)op->ob_ref_local;
        op->ob_ref_local = 0;  // restore ob_ref_local

        _PyObject_ASSERT(op, gc_refs >= 0);

        if (gc_is_unreachable(op) && gc_refs > 0) {
            // Clear the unreachable flag on any transitively reachable objects
            // from this one.
            any_resurrected = true;
            gc_clear_unreachable(op);
            if (mark_reachable(op) < 0) {
                return -1;
            }
        }
    }

    if (any_resurrected) {
        // Remove resurrected objects from the unreachable list.
        WORKSTACK_FOR_EACH_ITER(&state->unreachable, &iter, op) {
            if (!gc_is_unreachable(op)) {
                _PyObject_ASSERT(op, Py_REFCNT(op) > 1);
                worklist_remove(&iter);
                merge_refcount(op, -1);  // remove worklist reference
                state->long_lived_total++;
            }
        }
    }

#ifdef GC_DEBUG
    WORKSTACK_FOR_EACH(&state->unreachable, op) {
        _PyObject_ASSERT(op, gc_is_unreachable(op));
        _PyObject_ASSERT(op, _PyObject_GC_IS_TRACKED(op));
        _PyObject_ASSERT(op, op->ob_ref_local == 0);
        _PyObject_ASSERT(op, _Py_REF_IS_MERGED(op->ob_ref_shared));
    }
#endif

    return 0;
}


/* Invoke progress callbacks to notify clients that garbage collection
 * is starting or stopping
 */
static void
invoke_gc_callback(PyThreadState *tstate, const char *phase,
                   int generation, Py_ssize_t collected,
                   Py_ssize_t uncollectable, Py_ssize_t candidates,
                   double duration)
{
    assert(!_PyErr_Occurred(tstate));

    /* we may get called very early */
    GCState *gcstate = &tstate->interp->gc;
    if (gcstate->callbacks == NULL) {
        return;
    }

    /* The local variable cannot be rebound, check it for sanity */
    assert(PyList_CheckExact(gcstate->callbacks));
    if (PyList_GET_SIZE(gcstate->callbacks) == 0) {
        return;
    }

    PyObject *info = Py_BuildValue("{sisnsnsnsd}",
        "generation", generation,
        "collected", collected,
        "uncollectable", uncollectable,
        "candidates", candidates,
        "duration", duration);
    if (info == NULL) {
        PyErr_FormatUnraisable("Exception ignored while "
                               "invoking gc callbacks");
        return;
    }

    PyObject *phase_obj = PyUnicode_FromString(phase);
    if (phase_obj == NULL) {
        Py_DECREF(info);
        PyErr_FormatUnraisable("Exception ignored while "
                               "invoking gc callbacks");
        return;
    }

    PyObject *stack[] = {phase_obj, info};
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(gcstate->callbacks); i++) {
        PyObject *r, *cb = _PyList_GetItemRef((PyListObject *)gcstate->callbacks, i);
        if (cb == NULL) {
            break;
        }
        r = PyObject_Vectorcall(cb, stack, 2, NULL);
        if (r == NULL) {
            PyErr_FormatUnraisable("Exception ignored while "
                                   "calling GC callback %R", cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_DECREF(phase_obj);
    Py_DECREF(info);
    assert(!_PyErr_Occurred(tstate));
}

static void
cleanup_worklist(struct worklist *worklist)
{
    PyObject *op;
    while ((op = worklist_pop(worklist)) != NULL) {
        gc_clear_unreachable(op);
        Py_DECREF(op);
    }
}

// A full collection runs when the objects allocated (or, with generational
// collection, promoted) since the last one exceed 1/GC_HEAP_DIVISOR of the
// heap.
#define GC_HEAP_DIVISOR 4

// The net growth of the old generation since the last full collection:
// objects that became old, less old objects freed by reference counting
// (what is left is live old objects and old cyclic garbage).
static Py_ssize_t
gc_old_growth(GCState *gcstate)
{
    Py_ssize_t pending = _Py_atomic_load_ssize_relaxed(&gcstate->long_lived_pending);
    return Py_MAX(pending, 0);
}

// The memory on the side grew enough since the last collection to run one.
static bool
gc_side_bytes_exceeded(GCState *gcstate)
{
    if (!gc_generational_enabled() || gcstate->freeze_active) {
        return false;
    }
    Py_ssize_t grown = _Py_atomic_load_ssize_relaxed(&gcstate->side_bytes_new);
    if (grown < GC_SIDE_BYTES_MIN) {
        return false;
    }
    Py_ssize_t now = _Py_atomic_load_ssize_relaxed(&gcstate->side_bytes);
    Py_ssize_t before = Py_MAX(now - grown, 0);
    return grown > before / 100 * GC_SIDE_BYTES_PERCENT;
}

static bool
gc_should_collect(GCState *gcstate)
{
    int count = _Py_atomic_load_int_relaxed(&gcstate->young.count);
    int threshold = gcstate->young.threshold;
    int gc_enabled = _Py_atomic_load_int_relaxed(&gcstate->enabled);
    if (threshold != 0 && gc_enabled && gc_side_bytes_exceeded(gcstate)) {
        return true;
    }
    if (count <= threshold || threshold == 0 || !gc_enabled) {
        return false;
    }
    if (gcstate->old[0].threshold == 0) {
        // A few tests rely on immediate scheduling of the GC so we ignore the
        // extra conditions if generations[1].threshold is set to zero.
        return true;
    }
    if (gc_generational_enabled() && !gcstate->freeze_active) {
        // (After gc.freeze() every collection is a full one, so the
        // non-generational trigger below applies.)
        // Young collections are cheap: collect once enough objects have
        // been allocated, but never less often than the non-generational
        // policy would (on small heaps, a fixed young threshold lets
        // medium-lived garbage pile up).  (long_lived_total is only updated
        // by full collections; add what young collections promoted since,
        // or a growing heap looks small.)
        Py_ssize_t heap = gcstate->long_lived_total + gc_old_growth(gcstate);
        Py_ssize_t full_trigger = Py_MAX(threshold, heap / GC_HEAP_DIVISOR);
        Py_ssize_t young_trigger = Py_MAX(gcstate->young_trigger, GC_YOUNG_THRESHOLD);
        return count > Py_MIN(young_trigger, full_trigger);
    }
    if (count < gcstate->long_lived_total / GC_HEAP_DIVISOR) {
        // Avoid quadratic behavior by scaling threshold to the number of live
        // objects.
        return false;
    }
    return true;
}

static void
record_allocation(PyThreadState *tstate)
{
    struct _gc_thread_state *gc = &((_PyThreadStateImpl *)tstate)->gc;

    // We buffer the allocation count to avoid the overhead of atomic
    // operations for every allocation.
    gc->alloc_count++;
    if (gc->alloc_count >= LOCAL_ALLOC_COUNT_THRESHOLD) {
        // TODO: Use Py_ssize_t for the generation count.
        GCState *gcstate = &tstate->interp->gc;
        _Py_atomic_add_int(&gcstate->young.count, (int)gc->alloc_count);
        gc->alloc_count = 0;

        if (gc_should_collect(gcstate) &&
            !_Py_atomic_load_int_relaxed(&gcstate->collecting))
        {
            _Py_ScheduleGC(tstate);
        }
        else if (_Py_atomic_load_ptr_relaxed(&gcstate->delete_work) != NULL) {
            // Help free the garbage of the collection in progress.
            _Py_ScheduleGC(tstate);
        }
    }
}

static void
record_deallocation(PyThreadState *tstate)
{
    struct _gc_thread_state *gc = &((_PyThreadStateImpl *)tstate)->gc;

    gc->alloc_count--;
    if (gc->alloc_count <= -LOCAL_ALLOC_COUNT_THRESHOLD) {
        GCState *gcstate = &tstate->interp->gc;
        int count = _Py_atomic_load_int_relaxed(&gcstate->young.count);
        int new_count;
        do {
            if (count == 0) {
                break;
            }
            new_count = count + (int)gc->alloc_count;
            if (new_count < 0) {
                new_count = 0;
            }
        } while (!_Py_atomic_compare_exchange_int(&gcstate->young.count,
                                                  &count,
                                                  new_count));
        gc->alloc_count = 0;
    }
}

// An old object is freed outside of a collection.
static void
record_old_deallocation(PyThreadState *tstate)
{
    struct _gc_thread_state *gc = &((_PyThreadStateImpl *)tstate)->gc;
    gc->old_freed++;
    if (gc->old_freed >= LOCAL_ALLOC_COUNT_THRESHOLD) {
        // Like the allocation count, the growth doesn't go below zero: a
        // shrinking old generation doesn't postpone the next full collection.
        GCState *gcstate = &tstate->interp->gc;
        Py_ssize_t pending = _Py_atomic_load_ssize_relaxed(&gcstate->long_lived_pending);
        Py_ssize_t new_pending;
        do {
            if (pending <= 0) {
                break;
            }
            new_pending = Py_MAX(pending - gc->old_freed, 0);
        } while (!_Py_atomic_compare_exchange_ssize(&gcstate->long_lived_pending,
                                                    &pending, new_pending));
        gc->old_freed = 0;
    }
}

// The memory on the side that survived the collections since the last full
// one (held by objects that are old now, garbage or not) grew enough to look
// at the old generation.  Without this, what the objects in flight at each
// young collection hold piles up there: they survive it, become old, and
// their number alone doesn't bring on a full collection for a long time.
static bool
gc_side_bytes_old_growth_exceeded(GCState *gcstate)
{
    Py_ssize_t now = _Py_atomic_load_ssize_relaxed(&gcstate->side_bytes);
    Py_ssize_t grown = _Py_atomic_load_ssize_relaxed(&gcstate->side_bytes_new);
    Py_ssize_t base = Py_MAX(gcstate->side_bytes_at_full, 0);
    Py_ssize_t survived = now - grown - base;
    return (survived >= GC_SIDE_BYTES_MIN &&
            survived > base / 100 * GC_SIDE_BYTES_PERCENT);
}

// With generational collection, collections of generation 0 (automatic ones
// and gc.collect(0)) are young collections, unless the old generation grew
// enough since the last full collection.
static bool
gc_is_young_collection(GCState *gcstate, int generation, _PyGC_Reason reason)
{
    return (gc_generational_enabled() && generation == 0 &&
            reason != _Py_GC_REASON_SHUTDOWN &&
            !gcstate->freeze_active &&
            (reason == _Py_GC_REASON_MANUAL ||
             (gc_old_growth(gcstate) < gcstate->long_lived_total / GC_HEAP_DIVISOR &&
              !gc_side_bytes_old_growth_exceeded(gcstate))));
}

// After freeing a lot of garbage, return the empty heap pages of all threads
// so the memory can be reused elsewhere or released to the OS: after
// explicit gc.collect() calls.
static int
gc_should_trim(struct collection_state *state)
{
    if (state->reason != _Py_GC_REASON_MANUAL) {
        return 0;
    }
    // Only when this collection freed enough, or enough threads have exited
    // (leaving abandoned segments) since the last trim, to be worth it:
    // programs (and test suites) that call gc.collect() in a loop shouldn't
    // pay for a trim every time.
    static size_t abandoned_at_last_trim = 0;
    size_t abandoned = mi_atomic_load_relaxed(
        &state->interp->mimalloc.abandoned_pool.abandoned_count);
    if (state->collected >= 10000 ||
        abandoned >= abandoned_at_last_trim + 8)
    {
        abandoned_at_last_trim = abandoned;
        return 1;
    }
    if (abandoned < abandoned_at_last_trim) {
        abandoned_at_last_trim = abandoned;
    }
    return 0;
}

static void
gc_collect_internal(PyInterpreterState *interp, struct collection_state *state, int generation)
{
#ifdef GC_PHASE_TRACE
    (void)PyTime_PerfCounterRaw(&state->phase_last);
#endif
    _PyEval_StopTheWorld(interp);
    GC_PHASE_DONE(state, GC_PHASE_STW1_WAIT);

    // update collection and allocation counters
    if (generation+1 < NUM_GENERATIONS) {
        state->gcstate->old[generation].count += 1;
    }

    state->gcstate->young.count = 0;
    _Py_atomic_store_ssize_relaxed(&state->gcstate->side_bytes_new, 0);
    for (int i = 1; i <= generation; ++i) {
        state->gcstate->old[i-1].count = 0;
    }

#ifdef GC_DEBUG
    // Before we start, check that the heap is in a good condition. There must
    // be no objects with a zero reference count. And `ob_tid` must only have a
    // thread if the refcount is unmerged.
    gc_visit_heaps(interp, &validate_refcounts, &state->base);
#endif

    _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
        _PyThreadStateImpl *tstate = (_PyThreadStateImpl *)p;

        // merge per-thread refcount for types into the type's actual refcount
        _PyObject_MergePerThreadRefcounts(tstate);

        // merge refcounts for all queued objects
        merge_queued_objects(tstate, state);
    }
    _Py_FOR_EACH_TSTATE_END(interp);
    GC_PHASE_DONE(state, GC_PHASE_MERGE);

    process_delayed_frees(interp, state);
    GC_PHASE_DONE(state, GC_PHASE_DELAYED);

    // The previous collection freed a lot of memory, mostly on behalf of
    // other threads.  Return the pages that became empty.  This collects the
    // blocks other threads freed, which the heap walk below would otherwise
    // have to do.
    bool trimmed = false;
    if (state->gcstate->trim_pending) {
        state->gcstate->trim_pending = 0;
        _PyMem_TrimHeaps(interp, 0);
        trimmed = true;
    }
    GC_PHASE_DONE(state, GC_PHASE_TRIM_PENDING);

    #ifdef GC_ENABLE_MARK_ALIVE
    // If gc.freeze() was used, it seems likely that doing this "mark alive"
    // pass will not be a performance win.  Typically the majority of alive
    // objects will be marked as frozen and will be skipped anyhow, without
    // doing this extra work.  Doing this pass also defeats one of the
    // purposes of using freeze: avoiding writes to objects that are frozen.
    // So, we just skip this if gc.freeze() was used.
    if (!state->gcstate->freeze_active && !state->young) {
        // Mark objects reachable from known roots as "alive".  These will
        // be ignored for rest of the GC pass.  (Not in young collections:
        // it would traverse the old objects too.)
        int err = gc_mark_alive_from_roots(interp, state);
        if (err < 0) {
            _PyEval_StartTheWorld(interp);
            PyErr_NoMemory();
            return;
        }
    }
    #endif
    GC_PHASE_DONE(state, GC_PHASE_MARK_ALIVE);

    // Find unreachable objects
    int err = deduce_unreachable_heap(interp, state);
    if (err < 0) {
        _PyEval_StartTheWorld(interp);
        PyErr_NoMemory();
        return;
    }

#ifdef GC_DEBUG
    // At this point, no object should have the alive bit set
    gc_visit_heaps(interp, &validate_alive_bits, &state->base);
#endif

    // Print debugging information.
    if (interp->gc.debug & _PyGC_DEBUG_COLLECTABLE) {
        PyObject *op;
        WORKSTACK_FOR_EACH(&state->unreachable, op) {
            debug_cycle("collectable", op);
        }
    }

    if (gc_generational_enabled()) {
        gc_move_pages(state);
        if (state->young) {
            // The objects that became old since the last full collection.
            _Py_atomic_add_ssize(&state->gcstate->long_lived_pending, state->aged);
            // Adapt how often young collections run to what they find.
            GCState *gs = state->gcstate;
            int trigger = Py_MAX(gs->young_trigger, GC_YOUNG_THRESHOLD);
            if (state->candidates == 0) {
                // nothing to learn from
            }
            else if (state->aged * 100 >= state->candidates * GC_YOUNG_SURVIVAL_HIGH) {
                trigger = Py_MIN(trigger * 2, GC_YOUNG_THRESHOLD_MAX);
            }
            else if (state->aged * 100 < state->candidates * GC_YOUNG_SURVIVAL_LOW) {
                trigger = Py_MAX(trigger / 2, GC_YOUNG_THRESHOLD);
            }
            gs->young_trigger = trigger;
        }
        else {
            _Py_atomic_store_ssize_relaxed(&state->gcstate->long_lived_pending, 0);
        }
    }

    // Find weakref callbacks we will honor (but do not call them).
    if (state->has_weakrefs) {
        find_weakref_callbacks(state);
    }
    GC_PHASE_DONE(state, GC_PHASE_WEAKREFS);

    // Unreachable objects can only be resurrected by running their
    // finalizers or weakref callbacks, or through a weakref to them (which
    // other threads may call once the world is restarted).  If there are
    // none of those, finish up in this pause and skip the second one.
    bool may_resurrect = state->has_weakrefs || state->has_finalizers;
    if (may_resurrect) {
        // Objects removed from the worklist as resurrected could be freed
        // before delete_garbage() runs, so the array can't be used.
        gc_obj_array_clear(&state->garbage);
    }
    else {
        _PyGC_ClearAllFreeLists(interp);
        // Record the number of live GC objects (a young collection only
        // saw the young ones)
        if (!state->young) {
            interp->gc.long_lived_total = state->long_lived_total;
        }
    }
    _PyEval_StartTheWorld(interp);
    (void)trimmed;

    // Deallocate any object from the refcount merge step
    cleanup_worklist(&state->objs_to_decref);

    if (may_resurrect) {
        // Call weakref callbacks and finalizers after unpausing other
        // threads to avoid potential deadlocks.
        call_weakref_callbacks(state);
        finalize_garbage(state);
        GC_PHASE_DONE(state, GC_PHASE_FINALIZE);

        _PyEval_StopTheWorld(interp);
        GC_PHASE_DONE(state, GC_PHASE_STW2_WAIT);
        // Handle any objects that may have resurrected after the finalization.
        err = handle_resurrected_objects(state);
        // Clear free lists in all threads
        _PyGC_ClearAllFreeLists(interp);
        if (err == 0) {
            clear_weakrefs(state);
        }
        // Record the number of live GC objects (a young collection only
        // saw the young ones)
        if (!state->young) {
            interp->gc.long_lived_total = state->long_lived_total;
        }
        GC_PHASE_DONE(state, GC_PHASE_RESURRECT);
        _PyEval_StartTheWorld(interp);

        if (err < 0) {
            cleanup_worklist(&state->unreachable);
            cleanup_worklist(&state->legacy_finalizers);
            cleanup_worklist(&state->wrcb_to_call);
            cleanup_worklist(&state->objs_to_decref);
            PyErr_NoMemory();
            return;
        }
    }
    else {
        GC_PHASE_DONE(state, GC_PHASE_FINALIZE);
    }

    // Call tp_clear on objects in the unreachable set. This will cause
    // the reference cycles to be broken. It may also cause some objects
    // to be freed.
    delete_garbage(state);
    GC_PHASE_DONE(state, GC_PHASE_DELETE);

    // Append objects with legacy finalizers to the "gc.garbage" list.
    handle_legacy_finalizers(state);

    if (gc_should_trim(state)) {
        if (state->reason == _Py_GC_REASON_MANUAL) {
            // An explicit gc.collect(): return the memory now.
            _PyEval_StopTheWorld(interp);
            _PyMem_TrimHeaps(interp, 1);
            _PyEval_StartTheWorld(interp);
            GC_PHASE_DONE(state, GC_PHASE_TRIM);
            _PyMem_TrimArenas();
            GC_PHASE_DONE(state, GC_PHASE_PURGE);
        }
        else {
            // Avoid another pause: trim at the start of the next collection.
            state->gcstate->trim_pending = 1;
        }
    }
}

static struct gc_generation_stats *
get_stats(GCState *gcstate, int gen)
{
    if (gen == 0) {
        struct gc_young_stats_buffer *buffer = &gcstate->generation_stats->young;
        struct gc_generation_stats *stats = &buffer->items[buffer->index];
        return stats;
    }
    else {
        struct gc_old_stats_buffer *buffer = &gcstate->generation_stats->old[gen - 1];
        struct gc_generation_stats *stats = &buffer->items[buffer->index];
        return stats;
    }
}

/* This is the main function.  Read this to understand how the
 * collection process works. */
static Py_ssize_t
gc_collect_main(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    Py_ssize_t m = 0; /* # objects collected */
    Py_ssize_t n = 0; /* # unreachable objects that couldn't be collected */
    GCState *gcstate = &tstate->interp->gc;

    // gc_collect_main() must not be called before _PyGC_Init
    // or after _PyGC_Fini()
    assert(gcstate->garbage != NULL);
    assert(!_PyErr_Occurred(tstate));

    int expected = 0;
    if (!_Py_atomic_compare_exchange_int(&gcstate->collecting, &expected, 1)) {
        // Don't start a garbage collection if one is already in progress.
        return 0;
    }

    if (reason == _Py_GC_REASON_HEAP && !gc_should_collect(gcstate)) {
        // Don't collect if the threshold is not exceeded.
        _Py_atomic_store_int(&gcstate->collecting, 0);
        return 0;
    }
    gcstate->frame = tstate->current_frame;

    assert(generation >= 0 && generation < NUM_GENERATIONS);

    bool young = gc_is_young_collection(gcstate, generation, reason);
    if (gc_generational_enabled() && generation == 0 && !young) {
        // A collection of generation 0 that turns into a full collection is
        // reported (to gc.callbacks, the stats and the debug output) as one
        // of the oldest generation.
        generation = NUM_GENERATIONS - 1;
    }

#ifdef Py_STATS
    PyStats *s = _PyStats_GET();
    if (s) {
        s->object_stats.object_visits = 0;
    }
#endif
    GC_STAT_ADD(generation, collections, 1);

    if (reason != _Py_GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "start", generation, 0, 0, 0, 0.0);
    }

    if (gcstate->debug & _PyGC_DEBUG_STATS) {
        PySys_WriteStderr("gc: collecting generation %d...\n", generation);
        show_stats_each_generations(gcstate);
    }

    if (PyDTrace_GC_START_ENABLED()) {
        PyDTrace_GC_START(generation);
    }
    PyTime_t start, stop;
    (void)PyTime_PerfCounterRaw(&start);

    PyInterpreterState *interp = tstate->interp;

    struct collection_state state = {
        .interp = interp,
        .gcstate = gcstate,
        .reason = reason,
        .young = young,
    };

    gc_collect_internal(interp, &state, generation);
    if (!young) {
        gcstate->side_bytes_at_full =
            _Py_atomic_load_ssize_relaxed(&gcstate->side_bytes) -
            _Py_atomic_load_ssize_relaxed(&gcstate->side_bytes_new);
    }
    // (Only left over when the collection failed before moving pages.)
    PyMem_RawFree(state.promote.pages);
    PyMem_RawFree(state.demote.pages);
    gc_obj_array_clear(&state.garbage);

    m = state.collected;
    n = state.uncollectable;
#ifdef GC_PHASE_TRACE
    gc_phase_report(&state);
#endif

    (void)PyTime_PerfCounterRaw(&stop);
    double duration = PyTime_AsSecondsDouble(stop - start);

    if (gcstate->debug & _PyGC_DEBUG_STATS) {
        PySys_WriteStderr(
            "gc: done, %zd unreachable, %zd uncollectable, %.4fs elapsed\n",
            n+m, n, duration);
    }

    // Clear the current thread's free-list again.
    _PyThreadStateImpl *tstate_impl = (_PyThreadStateImpl *)tstate;
    _PyObject_ClearFreeLists(&tstate_impl->freelists, 0);

    if (_PyErr_Occurred(tstate)) {
        if (reason == _Py_GC_REASON_SHUTDOWN) {
            _PyErr_Clear(tstate);
        }
        else {
            PyErr_FormatUnraisable("Exception ignored in garbage collection");
        }
    }

    /* Update stats. */
    PyMutex_Lock(&gcstate->stats_mutex);
    struct gc_generation_stats *stats = get_stats(gcstate, generation);
    stats->ts_start = start;
    stats->ts_stop = stop;
    stats->collections++;
    stats->collected += m;
    stats->uncollectable += n;
    stats->duration += duration;
    stats->candidates += state.candidates;
    PyMutex_Unlock(&gcstate->stats_mutex);

    GC_STAT_ADD(generation, objects_collected, m);
#ifdef Py_STATS
    {
        PyStats *s = _PyStats_GET();
        if (s) {
            GC_STAT_ADD(generation, object_visits,
                s->object_stats.object_visits);
            s->object_stats.object_visits = 0;
        }
    }
#endif

    if (PyDTrace_GC_DONE_ENABLED()) {
        PyDTrace_GC_DONE(n + m);
    }

    if (reason != _Py_GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "stop", generation, m, n, state.candidates, duration);
    }

    assert(!_PyErr_Occurred(tstate));
    gcstate->frame = NULL;
    _Py_atomic_store_int(&gcstate->collecting, 0);
    return n + m;
}

static PyObject *
list_from_object_stack(_PyObjectStack *stack)
{
    PyObject *list = PyList_New(_PyObjectStack_Size(stack));
    if (list == NULL) {
        PyObject *op;
        while ((op = _PyObjectStack_Pop(stack)) != NULL) {
            Py_DECREF(op);
        }
        return NULL;
    }

    PyObject *op;
    Py_ssize_t idx = 0;
    while ((op = _PyObjectStack_Pop(stack)) != NULL) {
        assert(idx < PyList_GET_SIZE(list));
        PyList_SET_ITEM(list, idx++, op);
    }
    assert(idx == PyList_GET_SIZE(list));
    return list;
}

struct get_referrers_args {
    struct visitor_args base;
    PyObject *objs;
    _PyObjectStack results;
};

static int
referrersvisit(PyObject* obj, void *arg)
{
    PyObject *objs = arg;
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(objs); i++) {
        if (PyTuple_GET_ITEM(objs, i) == obj) {
            return 1;
        }
    }
    return 0;
}

static bool
visit_get_referrers(const mi_heap_t *heap, const mi_heap_area_t *area,
                    void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, true);
    if (op == NULL) {
        return true;
    }
    if (op->ob_gc_bits & (_PyGC_BITS_UNREACHABLE | _PyGC_BITS_FROZEN)) {
        // Exclude unreachable objects (in-progress GC) and frozen
        // objects from gc.get_objects() to match the default build.
        return true;
    }

    struct get_referrers_args *arg = (struct get_referrers_args *)args;
    if (op == arg->objs) {
        // Don't include the tuple itself in the referrers list.
        return true;
    }
    if (Py_TYPE(op)->tp_traverse(op, referrersvisit, arg->objs)) {
        if (_PyObjectStack_Push(&arg->results, Py_NewRef(op)) < 0) {
            return false;
        }
    }

    return true;
}

PyObject *
_PyGC_GetReferrers(PyInterpreterState *interp, PyObject *objs)
{
    // NOTE: We can't append to the PyListObject during gc_visit_heaps()
    // because PyList_Append() may reclaim an abandoned mimalloc segments
    // while we are traversing them.
    struct get_referrers_args args = { .objs = objs };
    _PyEval_StopTheWorld(interp);
    int err = gc_visit_heaps(interp, &visit_get_referrers, &args.base);
    _PyEval_StartTheWorld(interp);

    PyObject *list = list_from_object_stack(&args.results);
    if (err < 0) {
        PyErr_NoMemory();
        Py_CLEAR(list);
    }
    return list;
}

struct get_objects_args {
    struct visitor_args base;
    _PyObjectStack objects;
};

static bool
visit_get_objects(const mi_heap_t *heap, const mi_heap_area_t *area,
                  void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, true);
    if (op == NULL) {
        return true;
    }
    if (op->ob_gc_bits & (_PyGC_BITS_UNREACHABLE | _PyGC_BITS_FROZEN)) {
        // Exclude unreachable objects (in-progress GC) and frozen
        // objects from gc.get_objects() to match the default build.
        return true;
    }

    struct get_objects_args *arg = (struct get_objects_args *)args;
    if (_PyObjectStack_Push(&arg->objects, Py_NewRef(op)) < 0) {
        return false;
    }
    return true;
}

PyObject *
_PyGC_GetObjects(PyInterpreterState *interp, int generation)
{
    // NOTE: We can't append to the PyListObject during gc_visit_heaps()
    // because PyList_Append() may reclaim an abandoned mimalloc segments
    // while we are traversing them.
    struct get_objects_args args = { 0 };
    _PyEval_StopTheWorld(interp);
    int err = gc_visit_heaps(interp, &visit_get_objects, &args.base);
    _PyEval_StartTheWorld(interp);

    PyObject *list = list_from_object_stack(&args.objects);
    if (err < 0) {
        PyErr_NoMemory();
        Py_CLEAR(list);
    }
    return list;
}

static bool
visit_freeze(const mi_heap_t *heap, const mi_heap_area_t *area,
             void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, true);
    if (op != NULL && !gc_is_unreachable(op)) {
        op->ob_gc_bits |= _PyGC_BITS_FROZEN;
    }
    return true;
}

void
_PyGC_Freeze(PyInterpreterState *interp)
{
    struct visitor_args args;
    _PyEval_StopTheWorld(interp);
    GCState *gcstate = get_gc_state();
    gcstate->freeze_active = true;
    gc_visit_heaps(interp, &visit_freeze, &args);
    _PyEval_StartTheWorld(interp);
}

static bool
visit_unfreeze(const mi_heap_t *heap, const mi_heap_area_t *area,
               void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, true);
    if (op != NULL) {
        gc_clear_bit(op, _PyGC_BITS_FROZEN);
    }
    return true;
}

void
_PyGC_Unfreeze(PyInterpreterState *interp)
{
    struct visitor_args args;
    _PyEval_StopTheWorld(interp);
    GCState *gcstate = get_gc_state();
    gcstate->freeze_active = false;
    gc_visit_heaps(interp, &visit_unfreeze, &args);
    _PyEval_StartTheWorld(interp);
}

struct count_frozen_args {
    struct visitor_args base;
    Py_ssize_t count;
};

static bool
visit_count_frozen(const mi_heap_t *heap, const mi_heap_area_t *area,
                   void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, true);
    if (op != NULL && gc_is_frozen(op)) {
        struct count_frozen_args *arg = (struct count_frozen_args *)args;
        arg->count++;
    }
    return true;
}

Py_ssize_t
_PyGC_GetFreezeCount(PyInterpreterState *interp)
{
    struct count_frozen_args args = { .count = 0 };
    _PyEval_StopTheWorld(interp);
    gc_visit_heaps(interp, &visit_count_frozen, &args.base);
    _PyEval_StartTheWorld(interp);
    return args.count;
}

/* C API for controlling the state of the garbage collector */
int
PyGC_Enable(void)
{
    GCState *gcstate = get_gc_state();
    return _Py_atomic_exchange_int(&gcstate->enabled, 1);
}

int
PyGC_Disable(void)
{
    GCState *gcstate = get_gc_state();
    return _Py_atomic_exchange_int(&gcstate->enabled, 0);
}

int
PyGC_IsEnabled(void)
{
    GCState *gcstate = get_gc_state();
    return _Py_atomic_load_int_relaxed(&gcstate->enabled);
}

/* Public API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = &tstate->interp->gc;

    if (!_Py_atomic_load_int_relaxed(&gcstate->enabled)) {
        return 0;
    }

    Py_ssize_t n;
    PyObject *exc = _PyErr_GetRaisedException(tstate);
    n = gc_collect_main(tstate, NUM_GENERATIONS - 1, _Py_GC_REASON_MANUAL);
    _PyErr_SetRaisedException(tstate, exc);

    return n;
}

Py_ssize_t
_PyGC_Collect(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    return gc_collect_main(tstate, generation, reason);
}

void
_PyGC_CollectNoFail(PyThreadState *tstate)
{
    /* Ideally, this function is only called on interpreter shutdown,
       and therefore not recursively.  Unfortunately, when there are daemon
       threads, a daemon thread can start a cyclic garbage collection
       during interpreter shutdown (and then never finish it).
       See http://bugs.python.org/issue8713#msg195178 for an example.
       */
    gc_collect_main(tstate, NUM_GENERATIONS - 1, _Py_GC_REASON_SHUTDOWN);
}

void
_PyGC_DumpShutdownStats(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    if (!(gcstate->debug & _PyGC_DEBUG_SAVEALL)
        && gcstate->garbage != NULL && PyList_GET_SIZE(gcstate->garbage) > 0) {
        const char *message;
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            message = "gc: %zd uncollectable objects at shutdown";
        }
        else {
            message = "gc: %zd uncollectable objects at shutdown; " \
                "use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        }
        /* PyErr_WarnFormat does too many things and we are at shutdown,
           the warnings module's dependencies (e.g. linecache) may be gone
           already. */
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(gcstate->garbage)))
        {
            PyErr_FormatUnraisable("Exception ignored in GC shutdown");
        }
        if (gcstate->debug & _PyGC_DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(gcstate->garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr))) {
                PyErr_FormatUnraisable("Exception ignored in GC shutdown "
                                       "while formatting garbage");
            }
            else {
                PySys_WriteStderr(
                    "      %s\n",
                    PyBytes_AS_STRING(bytes)
                    );
            }
            Py_XDECREF(repr);
            Py_XDECREF(bytes);
        }
    }
}


void
_PyGC_Fini(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    Py_CLEAR(gcstate->garbage);
    Py_CLEAR(gcstate->callbacks);
    PyMem_RawFree(gcstate->generation_stats);
    gcstate->generation_stats = NULL;

    /* We expect that none of this interpreters objects are shared
       with other interpreters.
       See https://github.com/python/cpython/issues/90228. */
}

/* for debugging */

#ifdef Py_DEBUG
static int
visit_validate(PyObject *op, void *parent_raw)
{
    PyObject *parent = _PyObject_CAST(parent_raw);
    if (_PyObject_IsFreed(op)) {
        _PyObject_ASSERT_FAILED_MSG(parent,
                                    "PyObject_GC_Track() object is not valid");
    }
    return 0;
}
#endif


/* extension modules might be compiled with GC support so these
   functions must always be available */

void
PyObject_GC_Track(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_ASSERT_FAILED_MSG(op,
                                    "object already tracked "
                                    "by the garbage collector");
    }
    _PyObject_GC_TRACK(op);

#ifdef Py_DEBUG
    /* Check that the object is valid: validate objects traversed
       by tp_traverse() */
    traverseproc traverse = Py_TYPE(op)->tp_traverse;
    (void)traverse(op, visit_validate, op);
#endif
}

void
PyObject_GC_UnTrack(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    /* The code for some objects, such as tuples, is a bit
     * sloppy about when the object is tracked and untracked. */
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
    }
}

int
PyObject_IS_GC(PyObject *obj)
{
    return _PyObject_IS_GC(obj);
}

void
_Py_ScheduleGC(PyThreadState *tstate)
{
    if (!_Py_eval_breaker_bit_is_set(tstate, _PY_GC_SCHEDULED_BIT))
    {
        _Py_set_eval_breaker_bit(tstate, _PY_GC_SCHEDULED_BIT);
    }
}

void
_PyObject_GC_Link(PyObject *op)
{
    record_allocation(_PyThreadState_GET());
}

void
_Py_RunGC(PyThreadState *tstate)
{
    GCState *gcstate = &tstate->interp->gc;
    if (_Py_atomic_load_ptr_relaxed(&gcstate->delete_work) != NULL) {
        gc_assist_delete(tstate);
        return;
    }
    if (!PyGC_IsEnabled()) {
        return;
    }
    gc_collect_main(tstate, 0, _Py_GC_REASON_HEAP);
}

static PyObject *
gc_alloc(PyTypeObject *tp, size_t basicsize, size_t presize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (basicsize > PY_SSIZE_T_MAX - presize) {
        return _PyErr_NoMemory(tstate);
    }
    size_t size = presize + basicsize;
    char *mem = _PyObject_MallocWithType(tp, size);
    if (mem == NULL) {
        return _PyErr_NoMemory(tstate);
    }
    if (presize) {
        ((PyObject **)mem)[0] = NULL;
        ((PyObject **)mem)[1] = NULL;
    }
    PyObject *op = (PyObject *)(mem + presize);
    record_allocation(tstate);
    return op;
}

PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_SIZE(tp);
    if (_PyType_HasFeature(tp, Py_TPFLAGS_INLINE_VALUES)) {
        size += _PyInlineValuesSize(tp);
    }
    PyObject *op = gc_alloc(tp, size, presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_Init(op, tp);
    if (tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) {
        _PyObject_InitInlineValues(op, tp);
    }
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *)gc_alloc(tp, size, presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_InitVar(op, tp, nitems);
    return op;
}

PyObject *
PyUnstable_Object_GC_NewWithExtraData(PyTypeObject *tp, size_t extra_size)
{
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_SIZE(tp) + extra_size;
    PyObject *op = gc_alloc(tp, size, presize);
    if (op == NULL) {
        return NULL;
    }
    memset((char *)op + sizeof(PyObject), 0, size - sizeof(PyObject));
    _PyObject_Init(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    const size_t presize = _PyType_PreHeaderSize(((PyObject *)op)->ob_type);
    _PyObject_ASSERT((PyObject *)op, !_PyObject_GC_IS_TRACKED(op));
    if (basicsize > (size_t)PY_SSIZE_T_MAX - presize) {
        return (PyVarObject *)PyErr_NoMemory();
    }
    char *mem = (char *)op - presize;
    mem = (char *)_PyObject_ReallocWithType(Py_TYPE(op), mem,  presize + basicsize);
    if (mem == NULL) {
        return (PyVarObject *)PyErr_NoMemory();
    }
    op = (PyVarObject *) (mem + presize);
    Py_SET_SIZE(op, nitems);
    return op;
}

void
PyObject_GC_Del(void *op)
{
    size_t presize = _PyType_PreHeaderSize(((PyObject *)op)->ob_type);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
#ifdef Py_DEBUG
        PyObject *exc = PyErr_GetRaisedException();
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL,
                                     "Object of type %s is not untracked "
                                     "before destruction",
                                     Py_TYPE(op)->tp_name))
        {
            PyErr_FormatUnraisable("Exception ignored on object deallocation");
        }
        PyErr_SetRaisedException(exc);
#endif
    }

    PyThreadState *tstate = _PyThreadState_GET();
    record_deallocation(tstate);
    if (gc_has_bit((PyObject *)op, _PyGC_BITS_OLD)) {
        record_old_deallocation(tstate);
    }
    PyObject_Free(((char *)op)-presize);
}

int
PyObject_GC_IsTracked(PyObject* obj)
{
    return _PyObject_GC_IS_TRACKED(obj);
}

int
PyObject_GC_IsFinalized(PyObject *obj)
{
    return _PyGC_FINALIZED(obj);
}

struct custom_visitor_args {
    struct visitor_args base;
    gcvisitobjects_t callback;
    void *arg;
};

static bool
custom_visitor_wrapper(const mi_heap_t *heap, const mi_heap_area_t *area,
                       void *block, size_t block_size, void *args)
{
    PyObject *op = op_from_block(block, args, true);
    if (op == NULL) {
        return true;
    }

    struct custom_visitor_args *wrapper = (struct custom_visitor_args *)args;
    if (!wrapper->callback(op, wrapper->arg)) {
        return false;
    }

    return true;
}

void
_PyGC_VisitObjectsWorldStopped(PyInterpreterState *interp,
                               gcvisitobjects_t callback, void *arg)
{
    struct custom_visitor_args wrapper = {
        .callback = callback,
        .arg = arg,
    };
    gc_visit_heaps(interp, &custom_visitor_wrapper, &wrapper.base);
}

// Called by the allocator (Objects/obmalloc.c) when its estimate of the
// memory in use on the side has changed by `delta` bytes.  The estimate is
// net, like the object count: memory that reference counting frees again
// doesn't add up.  Returns false if there is no thread state to record the
// change with (the allocator then keeps it for later).
bool
_PyGC_HeapBytesChanged(Py_ssize_t delta)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        return false;
    }
    GCState *gcstate = &tstate->interp->gc;
    _Py_atomic_add_ssize(&gcstate->side_bytes, delta);
    Py_ssize_t grown = _Py_atomic_add_ssize(&gcstate->side_bytes_new, delta) + delta;
    if (grown < 0) {
        // Memory from before the last collection was freed: that isn't
        // credit for garbage made since.
        _Py_atomic_store_ssize_relaxed(&gcstate->side_bytes_new, 0);
    }
    if (delta > 0 && gc_side_bytes_exceeded(gcstate) &&
        gc_should_collect(gcstate) &&
        !_Py_atomic_load_int_relaxed(&gcstate->collecting))
    {
        _Py_ScheduleGC(tstate);
    }
    return true;
}

void
PyUnstable_GC_VisitObjects(gcvisitobjects_t callback, void *arg)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    _PyEval_StopTheWorld(interp);
    _PyGC_VisitObjectsWorldStopped(interp, callback, arg);
    _PyEval_StartTheWorld(interp);
}

/* Clear all free lists
 * All free lists are cleared during the collection of the highest generation.
 * Allocated items in the free list may keep a pymalloc arena occupied.
 * Clearing the free lists may give back memory to the OS earlier.
 * Free-threading version: Since freelists are managed per thread,
 * GC should clear all freelists by traversing all threads.
 */
void
_PyGC_ClearAllFreeLists(PyInterpreterState *interp)
{
    _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
        _PyThreadStateImpl *tstate = (_PyThreadStateImpl *)p;
        _PyObject_ClearFreeLists(&tstate->freelists, 0);
    }
    _Py_FOR_EACH_TSTATE_END(interp);
}

#endif  // Py_GIL_DISABLED
