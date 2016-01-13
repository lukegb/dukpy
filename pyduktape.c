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
static const char* DUKPY_CONTEXT_CAPSULE_NAME = "dukpy.dukcontext";
static const char* DUKPY_PTR_CAPSULE_NAME = "dukpy.miscpointer";

#define DUKPY_INTERNAL_PROPERTY "\xff\xff"

static int dukpy_wrap_a_python_object_somehow_and_return_it(duk_context *ctx, PyObject* obj);

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

    duk_context *ctx = (duk_context*)PyCapsule_GetPointer(pyctx, DUKPY_CONTEXT_CAPSULE_NAME);
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

static PyObject* dukpy_pyobj_from_stack(duk_context *ctx, int pos, PyObject* seen) {
    duk_dup(ctx, pos);
    void* kptr = duk_to_pointer(ctx, -1);
    duk_pop(ctx);
    PyObject* kkey = PyLong_FromVoidPtr(kptr);
    if (PyDict_Contains(seen, kkey)) {
        PyObject* ret = PyDict_GetItem(seen, kkey);
        Py_DECREF(kkey);
        return ret;
    }


    switch (duk_get_type(ctx, pos)) {
        case DUK_TYPE_UNDEFINED:
        case DUK_TYPE_NULL:
        {
            Py_DECREF(kkey);
            Py_RETURN_NONE;
        }

        case DUK_TYPE_BOOLEAN:
        {
            Py_DECREF(kkey);
            int val = duk_get_boolean(ctx, pos);
            if (val) {
                Py_RETURN_TRUE;
            } else {
                Py_RETURN_FALSE;
            }
        }

        case DUK_TYPE_NUMBER:
        {
            Py_DECREF(kkey);
            double val = duk_get_number(ctx, pos);
            return PyFloat_FromDouble(val);
        }

        case DUK_TYPE_STRING:
        {
            Py_DECREF(kkey);
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
                    PyList_SET_ITEM(val, i, dukpy_pyobj_from_stack(ctx, -1, seen));
                    duk_pop(ctx);
                }
                Py_DECREF(kkey);
                return val;
            } else if (duk_is_object(ctx, pos)) {
                // check if it has a _ptr
                duk_get_prop_string(ctx, pos, DUKPY_INTERNAL_PROPERTY "_ptr");
                void* ptr = duk_get_pointer(ctx, -1);
                duk_pop(ctx);
                if (ptr != NULL) {
                    PyObject* val = ptr;
                    Py_INCREF(val);
                    return val;
                }

                PyObject* val = PyDict_New();
                PyDict_SetItem(seen, kkey, val);
                duk_enum(ctx, pos, DUK_ENUM_OWN_PROPERTIES_ONLY);
                while (duk_next(ctx, -1, 1)) {
                    PyObject* ikey = dukpy_pyobj_from_stack(ctx, -2, seen);
                    PyObject* ival = dukpy_pyobj_from_stack(ctx, -1, seen);
                    PyDict_SetItem(val, ikey, ival);
                    Py_DECREF(ikey);
                    Py_DECREF(ival);
                    duk_pop_2(ctx);
                }
                duk_pop(ctx);
                Py_DECREF(kkey);
                return val;
            } else {
                // we should never end up here, but let's be safe
                Py_DECREF(kkey);
                return PyUnicode_FromString(duk_safe_to_string(ctx, pos));
            }
        }

        case DUK_TYPE_BUFFER:
        {
            Py_DECREF(kkey);
            // hooray
            duk_size_t size = 0;
            void* val = duk_get_buffer(ctx, pos, &size);
            return PyBytes_FromStringAndSize((const char*)val, size);
        }

        case DUK_TYPE_POINTER:
        {
            Py_DECREF(kkey);
            // err
            void* val = duk_get_pointer(ctx, pos);
            return PyCapsule_New(val, DUKPY_PTR_CAPSULE_NAME, NULL);
        }

        case DUK_TYPE_LIGHTFUNC:
        default:
        {
            Py_DECREF(kkey);
            // ???
            return PyUnicode_FromString(duk_safe_to_string(ctx, pos));                    
        }
    }
}

volatile static PyObject* exc_cause = NULL;

static duk_ret_t dukpy_callable_finalizer(duk_context *ctx) {
    duk_get_prop_string(ctx, 0, DUKPY_INTERNAL_PROPERTY "_ptr");
    void* ptr = duk_require_pointer(ctx, -1);

    // PyObject* repr = PyObject_Repr(ptr);
    // printf("Finalizing %p - %s\n", ptr, PyUnicode_AsUTF8(repr));
    // Py_XDECREF(repr);

    Py_XDECREF((PyObject*)ptr);
    duk_pop_2(ctx);
    return 0;
}
static duk_ret_t dukpy_callable_handler(duk_context *ctx) { 
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
        PyObject* argObj = dukpy_pyobj_from_stack(ctx, -1, seen);
        duk_pop(ctx);
        PyTuple_SET_ITEM(argTuple, i, argObj);
    }
    Py_DECREF(seen);

    // call!
    PyObject* ret = PyObject_Call(fptr, argTuple, NULL);
    if (ret == NULL) {
        // something went wrong :(
        return DUK_RET_INTERNAL_ERROR;
    }

    if (dukpy_wrap_a_python_object_somehow_and_return_it(ctx, ret) == 0) {
        Py_XDECREF(ret);
        return 0;
    }

    return 1;
}
static void dukpy_create_pyptrobj(duk_context *ctx, PyObject* obj) {
    duk_push_pointer(ctx, obj); // [obj objptr]
    duk_put_prop_string(ctx, -2, DUKPY_INTERNAL_PROPERTY "_ptr"); // [obj]
    duk_push_c_function(ctx, dukpy_callable_finalizer, 1); // [obj finalizer]
    duk_set_finalizer(ctx, -2); // [obj]
}
static void dukpy_generate_callable_func(duk_context *ctx, PyObject* obj) {
    duk_peval_string(ctx, "(function(fnc, ptr) { return function() { var args = Array.prototype.slice.call(arguments); args.push(ptr); return fnc.apply(this, args) }; })"); // [wf]
    duk_push_c_function(ctx, dukpy_callable_handler, DUK_VARARGS); // [wf caller]
    dukpy_create_pyptrobj(ctx, obj); // [wf caller]
    duk_push_pointer(ctx, obj); // [wf caller ptr]
    duk_call(ctx, 2); // [wrapper]
    duk_push_pointer(ctx, obj); // [wrapper objptr]
    duk_put_prop_string(ctx, -2, DUKPY_INTERNAL_PROPERTY "_ptr"); // [wrapper]
}
static void dukpy_create_objwrap(duk_context *ctx) {
    duk_push_global_stash(ctx); // [obj gstash]
    duk_peval_string(ctx, "(function(obj, proxy) { return new Proxy(obj, proxy); })"); // [obj gstash wf]
    duk_dup(ctx, -3); // [obj gstash wf obj]
    duk_get_prop_string(ctx, -3, "pydukObjWrapper"); // [obj gstash wf obj proxyCalls]
    duk_call(ctx, 2); // [obj gstash proxy]
    duk_remove(ctx, -2); // [obj proxy]
    duk_remove(ctx, -2); // [proxy]
}

static int dukpy_wrap_a_python_object_somehow_and_return_it(duk_context *ctx, PyObject* obj) {
    if (PyUnicode_Check(obj)) {
        char* val = PyUnicode_AsUTF8(obj);
        duk_push_string(ctx, val);
    } else if (obj == Py_None) {
        duk_push_null(ctx);
    } else if (PyBool_Check(obj)) {
        if (PyObject_RichCompareBool(obj, Py_True, Py_EQ) == 1) {
            duk_push_true(ctx);
        } else {
            duk_push_false(ctx);
        }
    } else if (PyNumber_Check(obj)) {
        double val = PyFloat_AsDouble(obj);
        duk_push_number(ctx, val);
    } else if (PyCallable_Check(obj)) {
        dukpy_generate_callable_func(ctx, obj);
    } else {
        duk_push_object(ctx);
        dukpy_create_pyptrobj(ctx, obj);
        dukpy_create_objwrap(ctx);
        duk_push_pointer(ctx, obj); // [proxy objptr]
        duk_put_prop_string(ctx, -2, DUKPY_INTERNAL_PROPERTY "_ptr"); // [proxy]
    }
    return 1;
}

static void* dukpy_get_objwrap_pyobj(duk_context *ctx, int where) {
    duk_get_prop_string(ctx, where, DUKPY_INTERNAL_PROPERTY "_ptr");
    PyObject* v = (PyObject*)duk_require_pointer(ctx, -1);
    duk_pop(ctx);
    return v;
}
static duk_ret_t dukpy_objwrap_get(duk_context *ctx) {
    // arguments: [wrappedObj key recv]
    duk_pop(ctx); // we don't care about recv

    const char* name = duk_require_string(ctx, -1);
    duk_pop(ctx);

    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -1);
    duk_pop(ctx);

    PyObject* thing = PyObject_GetAttrString(v, name);
    if (thing == NULL) {
        PyErr_Clear();
        return 0;
    }

    return dukpy_wrap_a_python_object_somehow_and_return_it(ctx, thing);
}
static duk_ret_t dukpy_objwrap_set(duk_context *ctx) {
    // arguments: [wrappedObj key newVal recv]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -4);
    printf("set %p\n", v);
    return 0;
}
static duk_ret_t dukpy_objwrap_has(duk_context *ctx) {
    // arguments: [wrappedObj key]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -2);
    printf("has %p\n", v);
    return 0;
}
static duk_ret_t dukpy_objwrap_deleteProperty(duk_context *ctx) {
    // arguments: [wrappedObj key]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -2);
    printf("deleteProperty %p\n", v);
    return 0;
}
static duk_ret_t dukpy_objwrap_enumerate(duk_context *ctx) {
    // arguments: [wrappedObj]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -1);
    duk_pop(ctx);

    PyObject* itr = NULL;
    PyObject* item = NULL;
    int itemPos = 0;
    int usePositionAsKey = 0;
    int maskDunder = 0;

    duk_push_array(ctx);

    // if we're a mapping, we return the keys
    if (PyMapping_Check(v)) {
        itr = PyMapping_Keys(v);
        if (itr == NULL) {
            PyErr_Clear();
        }
    }

    if (itr == NULL && (PySequence_Check(v) || PyUnicode_Check(v))) {
        // If we're a sequence, we return the ints
        itr = PySequence_List(v);
        
        if (itr == NULL) {
            printf("boo\n");
            PyErr_Clear();
        } else {
            usePositionAsKey = 1;
        }
    }

    if (itr == NULL) {
        // Otherwise, use dir
        itr = PyObject_Dir(v);
        
        if (itr == NULL) {
            PyErr_Clear();
        } else {
            maskDunder = 1;
        }
    }

    if (itr == NULL) {
        return 1;
    }

    PyObject* realIter = PyObject_GetIter(itr);
    if (realIter == NULL) {
        Py_DECREF(itr);
        return 1;
    }

    while ((item = PyIter_Next(realIter)) != NULL) {
        if (usePositionAsKey) {
            duk_push_number(ctx, itemPos);
            duk_put_prop_index(ctx, -2, itemPos);
        } else {
            if (PyNumber_Check(item)) {
                double val = PyFloat_AsDouble(item);
                if (!PyErr_Occurred()) {
                    duk_push_number(ctx, val);
                } else {
                    duk_push_undefined(ctx);
                }
            } else if (PyUnicode_Check(item)) {
                char* val = PyUnicode_AsUTF8(item);
                if (maskDunder && val[0] == '_' && val[1] == '_') {
                    Py_DECREF(item);
                    continue;
                }
                if (!PyErr_Occurred()) {
                    duk_push_string(ctx, val);
                } else {
                    duk_push_undefined(ctx);
                }
            } else {
                duk_push_undefined(ctx);
            }
            duk_put_prop_index(ctx, -2, itemPos);
        }
        Py_DECREF(item);
        itemPos++;
    }
    Py_DECREF(itr);
    Py_DECREF(realIter);
    return 1;
}
static duk_ret_t dukpy_objwrap_ownKeys(duk_context *ctx) {
    return dukpy_objwrap_enumerate(ctx);
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

    // we need to set up our object wrapper here
    duk_push_global_stash(ctx); // [gstash]

    duk_push_object(ctx); // [gstash wrapper]

    // "get"potentially
    duk_push_c_lightfunc(ctx, dukpy_objwrap_get, 3, 3, 0); // [gstash wrapper "get"]
    duk_put_prop_string(ctx, -2, "get"); // [gstash wrapper]

    // "set"
    duk_push_c_lightfunc(ctx, dukpy_objwrap_set, 4, 4, 0); // [gstash wrapper "set"]
    duk_put_prop_string(ctx, -2, "set"); // [gstash wrapper]

    // "has"
    duk_push_c_lightfunc(ctx, dukpy_objwrap_has, 2, 2, 0); // [gstash wrapper "has"]
    duk_put_prop_string(ctx, -2, "has"); // [gstash wrapper]

    // "deleteProperty"
    duk_push_c_lightfunc(ctx, dukpy_objwrap_deleteProperty, 2, 2, 0); // [gstash wrapper "deleteProperty"]
    duk_put_prop_string(ctx, -2, "deleteProperty"); // [gstash wrapper]

    // "enumerate"
    duk_push_c_lightfunc(ctx, dukpy_objwrap_enumerate, 1, 1, 0); // [gstash wrapper "enumerate"]
    duk_put_prop_string(ctx, -2, "enumerate"); // [gstash wrapper]

    // "ownKeys"
    duk_push_c_lightfunc(ctx, dukpy_objwrap_ownKeys, 1, 1, 0); // [gstash wrapper "ownKeys"]
    duk_put_prop_string(ctx, -2, "ownKeys"); // [gstash wrapper]

    // and finalise up
    duk_put_prop_string(ctx, -2, "pydukObjWrapper"); // [gstash]
    duk_pop(ctx); // []

    PyObject* pyctx = PyCapsule_New(ctx, DUKPY_CONTEXT_CAPSULE_NAME, &dukpy_destroy_pyctx);

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

    duk_gc(ctx, 0);

    duk_push_string(ctx, vars);
    duk_json_decode(ctx, -1);
    duk_put_global_string(ctx, "dukpy");

    int res = duk_peval_string(ctx, command);
    if (res != 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(DukPyError, duk_safe_to_string(ctx, -1));
        }
        return NULL;
    }

    PyObject* seen = PyDict_New();
    PyObject* ret = dukpy_pyobj_from_stack(ctx, -1, seen);
    duk_pop(ctx);
    Py_DECREF(seen);

    return ret;
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
    Py_INCREF(callable);

    dukpy_generate_callable_func(ctx, callable);
    duk_put_global_string(ctx, callable_name);

    Py_RETURN_NONE;
}
static PyObject *DukPy_add_global_object(PyObject *self, PyObject *args) {
    PyObject *pyctx;
    const char *object_name;
    PyObject *object;

    if (!PyArg_ParseTuple(args, "OsO", &pyctx, &object_name, &object))
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

    if (!object) {
        PyErr_SetString(PyExc_ValueError, "must provide valid object");
        return NULL;
    }
    Py_INCREF(object);

    duk_push_object(ctx);
    dukpy_create_pyptrobj(ctx, object);
    dukpy_create_objwrap(ctx);
    duk_push_pointer(ctx, object); // [proxy objptr]
    duk_put_prop_string(ctx, -2, DUKPY_INTERNAL_PROPERTY "_ptr"); // [proxy]
    duk_put_global_string(ctx, object_name);

    Py_RETURN_NONE;
}

static PyMethodDef DukPy_methods[] = {
    {"new_context", DukPy_create_context, METH_NOARGS, "Create a new DukPy context."},
    {"ctx_eval_string", DukPy_eval_string_ctx, METH_VARARGS, "Run Javascript code from a string in a given context."},
    {"add_global_callable", DukPy_add_global_callable, METH_VARARGS, "Add a callable to the global context."},
    {"add_global_object", DukPy_add_global_object, METH_VARARGS, "Add an object to the global context."},
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
