#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <Python.h>
#include "duktape.h"

#define UNUSED(x) (void)(x)

#if PY_MAJOR_VERSION >= 3
#define CONDITIONAL_PY3(three, two) (three)
#else
#define CONDITIONAL_PY3(three, two) (two)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static PyObject *DukPyError;
static const char* DUK_CONTEXT_CAPSULE_NAME = "dukpy.dukcontext";

duk_ret_t stack_json_encode(duk_context *ctx) {
    const char *output = duk_json_encode(ctx, -1);
    duk_push_string(ctx, output);
    return 1;
}

static void* dukpy_malloc(void *udata, duk_size_t size) {
    UNUSED(udata);

    return PyMem_Malloc(size);
}
static void* dukpy_realloc(void *udata, void *ptr, duk_size_t size) {
    UNUSED(udata);
    
    return PyMem_Realloc(ptr, size);
}
static void dukpy_free(void *udata, void *ptr) {
    UNUSED(udata);

    PyMem_Free(ptr);
}
static void dukpy_fatal(duk_context *ctx, duk_errcode_t code, const char *msg) {
    PyErr_SetString(PyExc_RuntimeError, msg);
}
static duk_context* dukpy_ensure_valid_ctx(PyObject* pyctx) {
    if (!PyCapsule_CheckExact(pyctx)) {
        return NULL;
    }

    duk_context *ctx = (duk_context*)PyCapsule_GetPointer(pyctx, DUK_CONTEXT_CAPSULE_NAME);
    if (!ctx) {
        return NULL;
    }

    return ctx;
}

static void dukpy_destroy_pyctx(PyObject* pyctx) {
    duk_context *ctx = dukpy_ensure_valid_ctx(pyctx);

    if (!ctx) {
        return;
    }

    duk_destroy_heap(ctx);
}


static PyObject *DukPy_create_context(PyObject *self, PyObject *args) {
    duk_context *ctx = duk_create_heap(
        &dukpy_malloc,
        &dukpy_realloc,
        &dukpy_free,
        NULL,
        &dukpy_fatal
    );
    if (!ctx) {
        PyErr_SetString(PyExc_RuntimeError, "allocating duk_context");
        return NULL;
    }

    PyObject* pyctx = PyCapsule_New(ctx, DUK_CONTEXT_CAPSULE_NAME, &dukpy_destroy_pyctx);

    return pyctx;
}

static PyObject *DukPy_eval_string_ctx(PyObject *self, PyObject *args) {
    PyObject *pyctx;
    const char *command;
    const char *vars;

    if (!PyArg_ParseTuple(args, "Oss", &pyctx, &command, &vars))
        return NULL;

    duk_context *ctx = dukpy_ensure_valid_ctx(pyctx);
    if (!ctx) {
        PyErr_SetString(PyExc_ValueError, "must provide a duk_context");
        return NULL;
    }

    duk_push_string(ctx, vars);
    duk_json_decode(ctx, -1);
    duk_put_global_string(ctx, "dukpy");

    int res = duk_peval_string(ctx, command);
    if (res != 0) {
        PyErr_SetString(DukPyError, duk_safe_to_string(ctx, -1));
        return NULL;
    }
  
    duk_int_t rc = duk_safe_call(ctx, stack_json_encode, 1, 1);
    if (rc != DUK_EXEC_SUCCESS) { 
        PyErr_SetString(DukPyError, duk_safe_to_string(ctx, -1));
        return NULL;
    }

    const char *output = duk_get_string(ctx, -1);
    PyObject *result = Py_BuildValue(CONDITIONAL_PY3("y", "s"), output);
    duk_pop(ctx);
    return result;
}

static PyMethodDef DukPy_methods[] = {
    {"new_context", DukPy_create_context, METH_NOARGS, "Create a new DukPy context."},
    {"ctx_eval_string", DukPy_eval_string_ctx, METH_VARARGS, "Run Javascript code from a string in a given context."},
    {NULL, NULL, 0, NULL}
};

static char DukPy_doc[] = "Provides Javascript support to Python through the duktape library.";


#if PY_MAJOR_VERSION >= 3

static struct PyModuleDef dukpymodule = {
    PyModuleDef_HEAD_INIT,
    "_dukpy",
    DukPy_doc,
    -1,
    DukPy_methods
};

PyMODINIT_FUNC 
PyInit__dukpy(void) 
{
    PyObject *module = PyModule_Create(&dukpymodule);
    if (module == NULL)
       return NULL;

    DukPyError = PyErr_NewException("_dukpy.JSRuntimeError", NULL, NULL);
    Py_INCREF(DukPyError);
    PyModule_AddObject(module, "JSRuntimeError", DukPyError);
    return module;
}

#else

PyMODINIT_FUNC 
init_dukpy()
{
    PyObject *module = Py_InitModule3("_dukpy", DukPy_methods, DukPy_doc);
    if (module == NULL)
       return;

    DukPyError = PyErr_NewException("_dukpy.JSRuntimeError", NULL, NULL);
    Py_INCREF(DukPyError);
    PyModule_AddObject(module, "JSRuntimeError", DukPyError);
}

#endif

#ifdef __cplusplus
}
#endif
