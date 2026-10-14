/* Extension module for GC benchmarks: types that behave like the ones in
 * real extension modules.
 *
 *   Node(size=0, pymem=False)
 *       A GC-tracked container with two object slots (.a and .b) and an
 *       optional "side" buffer of `size` bytes allocated with malloc(), or
 *       with PyMem_Malloc() if `pymem` is true (think of a numpy array, an
 *       image or a parser state). The GC can collect cycles of Nodes but
 *       does not know about the memory they hold on the side.
 *
 *   Holder(obj, size=0)
 *       A type *without* GC support that owns a reference to `obj`, like a C
 *       callback registration or a capsule. The GC can't see that reference,
 *       so a cycle that passes through a Holder is never collected unless the
 *       Holder is closed (.close()) or dropped.
 *
 *   register(obj), registry_get(i), registry_len(), clear_registry()
 *       Owned references kept in a C static global list that the module does
 *       not report to the GC (no m_traverse), like extension-level caches.
 *
 *   stats(), reset_peak()
 *       Side-buffer bytes currently allocated and the peak since the last
 *       reset, plus the number of live Nodes and Holders.
 */

#include "Python.h"
#include <stdlib.h>
#include <string.h>

static Py_ssize_t side_bytes;       // updated atomically
static Py_ssize_t side_peak;
static Py_ssize_t live_nodes;
static Py_ssize_t live_holders;
static PyObject *registry;          // owned, deliberately not traversed

static void
side_add(Py_ssize_t n)
{
    Py_ssize_t now = __atomic_add_fetch(&side_bytes, n, __ATOMIC_RELAXED);
    Py_ssize_t peak = __atomic_load_n(&side_peak, __ATOMIC_RELAXED);
    while (now > peak &&
           !__atomic_compare_exchange_n(&side_peak, &peak, now, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
    }
}

static void *
side_alloc(Py_ssize_t size, int pymem)
{
    if (size <= 0) {
        return NULL;
    }
    void *buf = pymem ? PyMem_Malloc(size) : malloc(size);
    if (buf == NULL) {
        return NULL;
    }
    memset(buf, 0xab, size);  // touch it, as real code would
    side_add(size);
    return buf;
}

static void
side_free(void *buf, Py_ssize_t size, int pymem)
{
    if (buf == NULL) {
        return;
    }
    if (pymem) {
        PyMem_Free(buf);
    }
    else {
        free(buf);
    }
    side_add(-size);
}

/* ---------------------------------------------------------------- Node */

typedef struct {
    PyObject_HEAD
    PyObject *a;
    PyObject *b;
    void *buf;
    Py_ssize_t size;
    int pymem;
} NodeObject;

static PyObject *
Node_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"size", "pymem", NULL};
    Py_ssize_t size = 0;
    int pymem = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|np:Node", kwlist,
                                     &size, &pymem)) {
        return NULL;
    }
    NodeObject *self = (NodeObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->buf = side_alloc(size, pymem);
    if (size > 0 && self->buf == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }
    self->size = size;
    self->pymem = pymem;
    __atomic_add_fetch(&live_nodes, 1, __ATOMIC_RELAXED);
    return (PyObject *)self;
}

static int
Node_traverse(PyObject *op, visitproc visit, void *arg)
{
    NodeObject *self = (NodeObject *)op;
    Py_VISIT(self->a);
    Py_VISIT(self->b);
    return 0;
}

static int
Node_clear(PyObject *op)
{
    NodeObject *self = (NodeObject *)op;
    Py_CLEAR(self->a);
    Py_CLEAR(self->b);
    return 0;
}

static void
Node_dealloc(PyObject *op)
{
    NodeObject *self = (NodeObject *)op;
    PyObject_GC_UnTrack(op);
    (void)Node_clear(op);
    side_free(self->buf, self->size, self->pymem);
    self->buf = NULL;
    __atomic_sub_fetch(&live_nodes, 1, __ATOMIC_RELAXED);
    Py_TYPE(op)->tp_free(op);
}

static PyObject *
Node_get_slot(PyObject *op, void *closure)
{
    NodeObject *self = (NodeObject *)op;
    PyObject *res;
    Py_BEGIN_CRITICAL_SECTION(op);
    res = closure ? self->b : self->a;
    res = res ? Py_NewRef(res) : Py_NewRef(Py_None);
    Py_END_CRITICAL_SECTION();
    return res;
}

static int
Node_set_slot(PyObject *op, PyObject *value, void *closure)
{
    NodeObject *self = (NodeObject *)op;
    PyObject *old;
    Py_BEGIN_CRITICAL_SECTION(op);
    PyObject **slot = closure ? &self->b : &self->a;
    old = *slot;
    *slot = Py_XNewRef(value);
    Py_END_CRITICAL_SECTION();
    Py_XDECREF(old);
    return 0;
}

static PyObject *
Node_get_size(PyObject *op, void *closure)
{
    return PyLong_FromSsize_t(((NodeObject *)op)->size);
}

static PyGetSetDef Node_getset[] = {
    {"a", Node_get_slot, Node_set_slot, NULL, NULL},
    {"b", Node_get_slot, Node_set_slot, NULL, (void *)1},
    {"size", Node_get_size, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject Node_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_gcbench_ext.Node",
    .tp_basicsize = sizeof(NodeObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = Node_new,
    .tp_traverse = Node_traverse,
    .tp_clear = Node_clear,
    .tp_dealloc = Node_dealloc,
    .tp_getset = Node_getset,
};

/* -------------------------------------------------------------- Holder */

typedef struct {
    PyObject_HEAD
    PyObject *obj;      // owned, not visible to the GC
    void *buf;
    Py_ssize_t size;
} HolderObject;

static PyObject *
Holder_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"obj", "size", NULL};
    PyObject *obj;
    Py_ssize_t size = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|n:Holder", kwlist,
                                     &obj, &size)) {
        return NULL;
    }
    HolderObject *self = (HolderObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->buf = side_alloc(size, 0);
    if (size > 0 && self->buf == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }
    self->size = size;
    self->obj = Py_NewRef(obj);
    __atomic_add_fetch(&live_holders, 1, __ATOMIC_RELAXED);
    return (PyObject *)self;
}

static void
Holder_dealloc(PyObject *op)
{
    HolderObject *self = (HolderObject *)op;
    Py_CLEAR(self->obj);
    side_free(self->buf, self->size, 0);
    __atomic_sub_fetch(&live_holders, 1, __ATOMIC_RELAXED);
    Py_TYPE(op)->tp_free(op);
}

static PyObject *
Holder_close(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    HolderObject *self = (HolderObject *)op;
    PyObject *old;
    Py_BEGIN_CRITICAL_SECTION(op);
    old = self->obj;
    self->obj = NULL;
    Py_END_CRITICAL_SECTION();
    Py_XDECREF(old);
    Py_RETURN_NONE;
}

static PyObject *
Holder_get_obj(PyObject *op, void *closure)
{
    HolderObject *self = (HolderObject *)op;
    PyObject *res;
    Py_BEGIN_CRITICAL_SECTION(op);
    res = self->obj ? Py_NewRef(self->obj) : Py_NewRef(Py_None);
    Py_END_CRITICAL_SECTION();
    return res;
}

static PyMethodDef Holder_methods[] = {
    {"close", Holder_close, METH_NOARGS, NULL},
    {NULL}
};

static PyGetSetDef Holder_getset[] = {
    {"obj", Holder_get_obj, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject Holder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_gcbench_ext.Holder",
    .tp_basicsize = sizeof(HolderObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,     // no Py_TPFLAGS_HAVE_GC on purpose
    .tp_new = Holder_new,
    .tp_dealloc = Holder_dealloc,
    .tp_methods = Holder_methods,
    .tp_getset = Holder_getset,
};

/* ------------------------------------------------------------- module */

static PyObject *
mod_register(PyObject *module, PyObject *obj)
{
    if (PyList_Append(registry, obj) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
mod_registry_get(PyObject *module, PyObject *arg)
{
    Py_ssize_t i = PyLong_AsSsize_t(arg);
    if (i == -1 && PyErr_Occurred()) {
        return NULL;
    }
    PyObject *item = PyList_GetItemRef(registry, i);
    return item;
}

static PyObject *
mod_registry_len(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromSsize_t(PyList_GET_SIZE(registry));
}

static PyObject *
mod_clear_registry(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    if (PyList_SetSlice(registry, 0, PY_SSIZE_T_MAX, NULL) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
mod_stats(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return Py_BuildValue(
        "{snsnsnsn}",
        "side_bytes", __atomic_load_n(&side_bytes, __ATOMIC_RELAXED),
        "side_peak", __atomic_load_n(&side_peak, __ATOMIC_RELAXED),
        "live_nodes", __atomic_load_n(&live_nodes, __ATOMIC_RELAXED),
        "live_holders", __atomic_load_n(&live_holders, __ATOMIC_RELAXED));
}

static PyObject *
mod_reset_peak(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    __atomic_store_n(&side_peak, __atomic_load_n(&side_bytes, __ATOMIC_RELAXED),
                     __ATOMIC_RELAXED);
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"register", mod_register, METH_O, NULL},
    {"registry_get", mod_registry_get, METH_O, NULL},
    {"registry_len", mod_registry_len, METH_NOARGS, NULL},
    {"clear_registry", mod_clear_registry, METH_NOARGS, NULL},
    {"stats", mod_stats, METH_NOARGS, NULL},
    {"reset_peak", mod_reset_peak, METH_NOARGS, NULL},
    {NULL}
};

static int
module_exec(PyObject *module)
{
    if (PyType_Ready(&Node_Type) < 0 || PyType_Ready(&Holder_Type) < 0) {
        return -1;
    }
    if (registry == NULL) {
        registry = PyList_New(0);
        if (registry == NULL) {
            return -1;
        }
    }
    if (PyModule_AddObjectRef(module, "Node", (PyObject *)&Node_Type) < 0 ||
        PyModule_AddObjectRef(module, "Holder", (PyObject *)&Holder_Type) < 0)
    {
        return -1;
    }
    return 0;
}

static PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, module_exec},
#ifdef Py_mod_gil
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_gcbench_ext",
    .m_size = 0,
    .m_methods = module_methods,
    .m_slots = module_slots,
};

PyMODINIT_FUNC
PyInit__gcbench_ext(void)
{
    return PyModuleDef_Init(&module_def);
}
