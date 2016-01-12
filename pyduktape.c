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
static const char* DUK_PTR_CAPSULE_NAME = "dukpy.miscpointer";

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

static PyObject* pyduk_pyobj_from_stack(duk_context *ctx, int pos, PyObject* seen) {
    duk_dup(ctx, pos);
    void* kptr = duk_to_pointer(ctx, -1);
    duk_pop(ctx);
    PyObject* kkey = PyLong_FromVoidPtr(kptr);
    if (PyDict_Contains(seen, kkey)) {
        return PyDict_GetItem(seen, kkey);
    }


    switch (duk_get_type(ctx, pos)) {
        case DUK_TYPE_UNDEFINED:
        case DUK_TYPE_NULL:
        {
            Py_RETURN_NONE;
        }

        case DUK_TYPE_BOOLEAN:
        {
            int val = duk_get_boolean(ctx, pos);
            if (val) {
                Py_RETURN_TRUE;
            } else {
                Py_RETURN_FALSE;
            }
        }

        case DUK_TYPE_NUMBER:
        {
            double val = duk_get_number(ctx, pos);
            return PyFloat_FromDouble(val);
        }

        case DUK_TYPE_STRING:
        {
            const char* val = duk_get_string(ctx, pos);
            return PyUnicode_FromString(val);
        }

        case DUK_TYPE_OBJECT:
        {
            if (duk_is_array(ctx, pos)) {
                int len = duk_get_length(ctx, pos);
                PyObject* val = PyList_New(len);
                PyDict_SetItem(seen, kkey, val);
                for (int i = 0; i < len; i++) {
                    duk_get_prop_index(ctx, pos, i);
                    PyList_SET_ITEM(val, i, pyduk_pyobj_from_stack(ctx, -1, seen));
                    duk_pop(ctx);
                }
                return val;
            } else if (duk_is_object(ctx, pos)) {
                PyObject* val = PyDict_New();
                PyDict_SetItem(seen, kkey, val);
                duk_enum(ctx, pos, DUK_ENUM_OWN_PROPERTIES_ONLY);
                while (duk_next(ctx, -1, 1)) {
                    PyObject* ikey = pyduk_pyobj_from_stack(ctx, -2, seen);
                    PyObject* ival = pyduk_pyobj_from_stack(ctx, -1, seen);
                    PyDict_SetItem(val, ikey, ival);
                    duk_pop_2(ctx);
                }
                duk_pop(ctx);
                return val;
            } else {
                // we should never end up here, but let's be safe
                return PyUnicode_FromString(duk_safe_to_string(ctx, pos));
            }
        }

        case DUK_TYPE_BUFFER:
        {
            // hooray
            duk_size_t size = 0;
            void* val = duk_get_buffer(ctx, pos, &size);
            return PyBytes_FromStringAndSize((const char*)val, size);
        }

        case DUK_TYPE_POINTER:
        {
            // err
            void* val = duk_get_pointer(ctx, pos);
            return PyCapsule_New(val, DUK_PTR_CAPSULE_NAME, NULL);
        }

        case DUK_TYPE_LIGHTFUNC:
        default:
        {
            // ???
            return PyUnicode_FromString(duk_safe_to_string(ctx, pos));                    
        }
    }
}

static duk_ret_t callable_finalizer(duk_context *ctx) {
    duk_get_prop_string(ctx, 0, "_ptr");
    void* ptr = duk_require_pointer(ctx, -1);
    Py_XDECREF((PyObject*)ptr);
    duk_pop_2(ctx);
    return 0;   
}
static duk_ret_t callable_handler(duk_context *ctx) { 
    void* ptr = duk_require_pointer(ctx, -1);
    duk_pop(ctx);

    PyObject* fptr = (PyObject*)ptr;
    if (!PyCallable_Check(fptr))
        return DUK_RET_REFERENCE_ERROR;

    // build argument tuple
    int nargs = duk_get_top(ctx);
    PyObject* argTuple = PyTuple_New(nargs);
    PyObject* seen = PyDict_New();
    for (int i = nargs - 1; i >= 0; i--) {
        PyObject* argObj = pyduk_pyobj_from_stack(ctx, -1, seen);
        duk_pop(ctx);
        PyTuple_SET_ITEM(argTuple, i, argObj);
    }

    // call!
    PyObject* ret = PyObject_Call(fptr, argTuple, NULL);
    if (ret == NULL) {
        // something went wrong :(
        return DUK_ERR_INTERNAL_ERROR;
    }
    Py_XDECREF(ret);

    return 0;
}
static void generate_callable_func(duk_context *ctx, PyObject* obj) {
    duk_peval_string(ctx, "(function(fnc, ptr) { return function() { var args = Array.prototype.slice.call(arguments); args.push(ptr); return fnc.apply(this, args) }; })");
    duk_push_c_function(ctx, callable_handler, DUK_VARARGS); // [caller]
    duk_push_c_function(ctx, callable_finalizer, 1); // [caller finalizer]
    duk_set_finalizer(ctx, -2); // [caller]
    duk_push_pointer(ctx, obj); // [caller ptr]
    duk_dup(ctx, -1); // [caller ptr ptr]
    duk_put_prop_string(ctx, -3, "_ptr"); // [caller ptr]
    duk_call(ctx, 2); // [wrapper]   
}

static PyObject *DukPy_add_global_callable(PyObject *self, PyObject *args) {
    PyObject *pyctx;
    const char *callable_name;
    PyObject *callable;

    if (!PyArg_ParseTuple(args, "OsO", &pyctx, &callable_name, &callable))
        return NULL;

    if (!pyctx) {
        PyErr_SetString(PyExc_ValueError, "must provide a duk_context");
        return NULL;
    }

    duk_context *ctx = dukpy_ensure_valid_ctx(pyctx);
    if (!ctx) {
        PyErr_SetString(PyExc_ValueError, "must provide a duk_context");
        return NULL;
    }

    if (!callable || !PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_ValueError, "must provide valid callable");
        return NULL;
    }

    generate_callable_func(ctx, callable);
    duk_put_global_string(ctx, callable_name);

    Py_RETURN_NONE;
}

static PyMethodDef DukPy_methods[] = {
    {"new_context", DukPy_create_context, METH_NOARGS, "Create a new DukPy context."},
    {"ctx_eval_string", DukPy_eval_string_ctx, METH_VARARGS, "Run Javascript code from a string in a given context."},
    {"add_global_callable", DukPy_add_global_callable, METH_VARARGS, "Add a callable to the global context."},
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
