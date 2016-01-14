#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <Python.h>
#include "structmember.h"
#include "duktape.h"

#define UNUSED(x) (void)(x)

#if PY_MAJOR_VERSION >= 3
#define CONDITIONAL_PY3(three, two) (three)
#define DUKPY_IS_NSTRING PyUnicode_Check
#define DUKPY_NSTRING_TO_CHAR PyUnicode_AsUTF8
#define DUKPY_CHAR_TO_NSTRING PyUnicode_FromString
#else
#define CONDITIONAL_PY3(three, two) (two)
#define DUKPY_IS_NSTRING PyString_Check
#define DUKPY_NSTRING_TO_CHAR PyString_AsString
#define DUKPY_CHAR_TO_NSTRING PyString_FromString
#endif


#ifdef DUKPY_DEBUG
#define DUKPY_DEBUG_PRINT printf
#define DUKPY_DEBUG_PRINT_REPR(thing) { do { PyObject* tptr = (thing); PyObject* repr = PyObject_Repr(tptr); printf("obj at %p: repr: %s\n", tptr, DUKPY_NSTRING_TO_CHAR(repr)); Py_DECREF(repr); } while (0); }
#else
#define DUKPY_DEBUG_PRINT(x, v...) // 
#define DUKPY_DEBUG_PRINT_REPR(thing) // 
#endif

#ifdef __cplusplus
extern "C" {
#endif

static PyObject *DukPyError;
static const char* DUKPY_CONTEXT_CAPSULE_NAME = "dukpy.dukcontext";
static const char* DUKPY_FUNCTION_CAPSULE_NAME = "dukpy.dukfunction";
static const char* DUKPY_PTR_CAPSULE_NAME = "dukpy.miscpointer";


#define DUKPY_INTERNAL_PROPERTY "\xff\xff"

struct DukPyFunction {
    duk_context* ctx;
    PyObject* pyctx;
    const char* name;
};

static int dukpy_wrap_a_python_object_somehow_and_return_it(duk_context *ctx, PyObject* obj);

static const char* dukpy_generate_random_name(duk_context* ctx, duk_size_t len) {
    len = (len < 7) ? 32 : len;
    char* name = calloc(sizeof(char), len);
    name[0] = '\xFF';
    name[1] = '\xFF';
    name[2] = 'd';
    name[3] = 'u';
    name[4] = 'k';
    name[5] = 'p';
    name[6] = 'y';

    FILE *f = fopen("/dev/urandom", "r");
    if (!f) {
        free(name);
        return NULL;
    }

    size_t read = fread(name+7, 1, len-7, f);
    if (read != len-7) {
        fclose(f);
        free(name);
        return NULL;
    }

    fclose(f);
    return name;
}
static struct DukPyFunction* dukpy_generate_function(duk_context* ctx) {
    struct DukPyFunction* dpf = calloc(sizeof(struct DukPyFunction*), 1);
    if (!dpf) {
        return NULL;
    }

    duk_push_global_stash(ctx); // [... gstash]
    int nameAlreadyExists = 1;
    int nameLen = 32;
    const char* newName = NULL;
    while (nameAlreadyExists) {
        newName = dukpy_generate_random_name(ctx, nameLen++);
        if (!newName) {
            duk_push_string(ctx, "?!?");
            duk_throw(ctx);
            return NULL;
        }

        duk_get_prop_string(ctx, -1, newName); // [... gstash nprop]
        if (duk_is_undefined(ctx, -1)) {
            nameAlreadyExists = 0;
        } else {
            free((void*)newName);
        }

        duk_pop(ctx); // [... gstash]
    }
    duk_get_prop_string(ctx, -1, "pydukPyCTX"); // [... gstash pyctx]
    PyObject* pyctx = (PyObject*)duk_require_pointer(ctx, -1); // [... gstash pyctx]
    Py_XINCREF(pyctx);
    duk_pop(ctx); // [... gstash]

    duk_pop(ctx); // [...]

    dpf->ctx = ctx;
    dpf->name = newName;
    return dpf;
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
    DUKPY_DEBUG_PRINT("destroying pyctx\n");
    duk_context *ctx = dukpy_ensure_valid_ctx(pyctx);

    if (!ctx) {
        return;
    }

    DUKPY_DEBUG_PRINT("OK, destroying PyJSFunction...\n");

    duk_push_global_stash(ctx); // [... gstash]
    duk_get_prop_string(ctx, -1, "pydukPyJSFunction"); // [... gstash ptr]
    PyObject* pyJSFunction = duk_get_pointer(ctx, -1); // [... gstash ptr]
    duk_del_prop_string(ctx, -2, "pydukPyJSFunction"); // [... gstash ptr]
    duk_pop_2(ctx); // [...]
    Py_XDECREF(pyJSFunction);

    DUKPY_DEBUG_PRINT("OK, destroying heap!\n");

    duk_destroy_heap(ctx);

    DUKPY_DEBUG_PRINT("We're outta here.");
}
static void dukpy_function_destructor(PyObject* pyfunc) {
    DUKPY_DEBUG_PRINT("destructing function here!\n");

    if (!PyCapsule_CheckExact(pyfunc)) {
        return;
    }

    struct DukPyFunction* dpf = (struct DukPyFunction*)PyCapsule_GetPointer(pyfunc, DUKPY_FUNCTION_CAPSULE_NAME);
    if (!dpf) {
        return;
    }

    if (!dpf->ctx) {
        free((void*)dpf->name);
        dpf->name = NULL;
        free((void*)dpf);
        return;
    }

    if (!dpf->name) {
        free((void*)dpf);
        return;
    }

    DUKPY_DEBUG_PRINT("destructing function\n");

    duk_push_global_stash(dpf->ctx); // [... gstash]
    duk_del_prop_string(dpf->ctx, -1, dpf->name); // [... gstash]
    duk_get_prop_string(dpf->ctx, -1, "pydukPyCTX"); // [... gstash pyctx]
    PyObject* pyctx = (PyObject*)duk_require_pointer(dpf->ctx, -1); // [... gstash pyctx]
    duk_pop(dpf->ctx); // [... gstash]
    duk_pop(dpf->ctx); // [...]

    free((void*)dpf->name);
    dpf->name = NULL;
    free((void*)dpf);

    Py_XDECREF(pyctx);
}

static PyObject* dukpy_pyobj_from_stack(duk_context *ctx, int pos, PyObject* seen, int hasWrapper, int wrapperPos) {
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

            // is this int-y enough for us to try and int cast?
            duk_push_string(ctx, "(function(n) { return Math.round(n) == n })");
            duk_peval(ctx);
            if (pos < 0) {
                duk_dup(ctx, pos - 1);
            } else {
                duk_dup(ctx, pos);
            }
            duk_call(ctx, 1);
            duk_bool_t res = duk_require_boolean(ctx, -1);
            duk_pop(ctx);

            if (res) {
                // try the int cast
                {
                    duk_int_t val = duk_get_int(ctx, pos);
                    if (val > DUK_INT_MIN && val < DUK_INT_MAX) {
                        return PyLong_FromLong(val);
                    }
                }

                // try the uint cast?
                {
                    duk_uint_t val = duk_get_uint(ctx, pos);
                    if (val > DUK_UINT_MIN && val < DUK_UINT_MAX) {
                        return PyLong_FromUnsignedLong(val);
                    }
                }
            }

            double val = duk_get_number(ctx, pos);
            return PyFloat_FromDouble(val);
        }

        case DUK_TYPE_STRING:
        {
            Py_DECREF(kkey);
            const char* val = duk_get_string(ctx, pos);
            return DUKPY_CHAR_TO_NSTRING(val);
        }

        case DUK_TYPE_OBJECT:
        {
            if (duk_is_function(ctx, pos)) {
                // hoo boy
                duk_dup(ctx, pos); // [... func]
                duk_push_global_stash(ctx); // [... func gstash]
                struct DukPyFunction* dpf = dukpy_generate_function(ctx);
                duk_dup(ctx, -2); // [... func gstash func]

                // we need to bind this function if it's not already bound
                if (hasWrapper && !duk_is_bound_function(ctx, -1)) {
                    // bind it!
                    int newWrapperPos = wrapperPos;
                    if (newWrapperPos < 0) {
                        newWrapperPos -= 4;
                    }
                    duk_push_string(ctx, "bind"); // [... func gstash func "bind"]
                    duk_dup(ctx, newWrapperPos); // [...func gstash func "bind" this]
                    duk_call_prop(ctx, -3, 1); //[...func gstash func funcTHIS]
                    duk_remove(ctx, -2); // [...func gstash funcTHIS]
                }

                duk_put_prop_string(ctx, -2, dpf->name); // [... func gstash]
                duk_pop_2(ctx); // [...]

                PyObject* capsule = PyCapsule_New((void*)dpf, DUKPY_FUNCTION_CAPSULE_NAME, dukpy_function_destructor);

                // we have the capsule, now create the wrapper
                duk_push_global_stash(ctx);
                duk_get_prop_string(ctx, -1, "pydukPyJSFunction"); // [... gstash JSFunction]
                PyObject* pyJSFunctionClazz = (PyObject*)duk_require_pointer(ctx, -1);
                duk_pop(ctx); // [... gstash]
                PyObject* pyJSFunctionArgList = Py_BuildValue("(O)", capsule);
                PyObject* pyJSFunction = PyObject_CallObject(pyJSFunctionClazz, pyJSFunctionArgList);
                Py_DECREF(pyJSFunctionArgList);
                Py_DECREF(capsule);
                duk_pop(ctx); // [...]

                PyDict_SetItem(seen, kkey, pyJSFunction);
                Py_DECREF(kkey);
                return pyJSFunction;
            } else if (duk_is_array(ctx, pos)) {
                int len = duk_get_length(ctx, pos);
                PyObject* val = PyList_New(len);
                PyDict_SetItem(seen, kkey, val);

                int myPosInside = pos;
                if (pos < 0) {
                    // account for the actual python object itself
                    myPosInside -= 1;
                }

                for (int i = 0; i < len; i++) {
                    duk_get_prop_index(ctx, pos, i);
                    PyList_SET_ITEM(val, i, dukpy_pyobj_from_stack(ctx, -1, seen, 1, myPosInside));
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

                int myPosInside = pos;
                if (pos < 0) {
                    // account for the enumerator, key and values
                    myPosInside -= 3;
                }

                PyObject* val = PyDict_New();
                PyDict_SetItem(seen, kkey, val);
                duk_enum(ctx, pos, DUK_ENUM_OWN_PROPERTIES_ONLY);
                while (duk_next(ctx, -1, 1)) {
                    PyObject* ikey = dukpy_pyobj_from_stack(ctx, -2, seen, 1, myPosInside);
                    PyObject* ival = dukpy_pyobj_from_stack(ctx, -1, seen, 1, myPosInside);
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
                return DUKPY_CHAR_TO_NSTRING(duk_safe_to_string(ctx, pos));
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
            return DUKPY_CHAR_TO_NSTRING(duk_safe_to_string(ctx, pos));                    
        }
    }
}

static duk_ret_t dukpy_callable_finalizer(duk_context *ctx) {
    duk_get_prop_string(ctx, 0, DUKPY_INTERNAL_PROPERTY "_ptr");
    void* ptr = duk_require_pointer(ctx, -1);

    DUKPY_DEBUG_PRINT("Finalizing %p:\n", ptr);
    DUKPY_DEBUG_PRINT_REPR(ptr);

    Py_XDECREF((PyObject*)ptr);
    duk_pop_2(ctx);

    DUKPY_DEBUG_PRINT("Finalizer on %p done.\n", ptr);
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
        PyObject* argObj = dukpy_pyobj_from_stack(ctx, -1, seen, 0, 0);
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


    DUKPY_DEBUG_PRINT("generating wrapper for %p\n", obj);
    DUKPY_DEBUG_PRINT_REPR(obj);


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
    if (DUKPY_IS_NSTRING(obj)) {
        char* val = DUKPY_NSTRING_TO_CHAR(obj);
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
static int dukpy_push_a_python_sequence_somehow_and_return_the_count(duk_context *ctx, PyObject* obj) {
    if (!PySequence_Check(obj)) {
        return -1;
    }

    PyObject* pymyarglist = PySequence_List(obj);

    int argCount = PySequence_Size(pymyarglist);
    for (int i = 0; i < argCount;) {
        PyObject* pythis = PySequence_GetItem(pymyarglist, i);
        int pushedThisTime = dukpy_wrap_a_python_object_somehow_and_return_it(ctx, pythis);
        if (pushedThisTime != 1) {
            PyErr_SetString(PyExc_ValueError, ":(");
            Py_DECREF(pymyarglist);
            Py_DECREF(pythis);
            return i;
        }
        i += pushedThisTime;
        Py_DECREF(pythis);
    }

    Py_DECREF(pymyarglist);

    return argCount;
}


static void* dukpy_get_objwrap_pyobj(duk_context *ctx, int where) {
    duk_get_prop_string(ctx, where, DUKPY_INTERNAL_PROPERTY "_ptr");
    PyObject* v = (PyObject*)duk_require_pointer(ctx, -1);
    duk_pop(ctx);
    return v;
}
static duk_ret_t dukpy_objwrap_toString(duk_context *ctx) {
    duk_push_this(ctx);
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -1);
    duk_pop(ctx);
    if (v == NULL) {
        return 0;
    }

    PyObject* vstr = PyObject_Str(v);
    if (v == NULL) {
        vstr = PyObject_Repr(v);
    }
    if (v == NULL) {
        return 0;
    }

    char* vstrutf8 = DUKPY_NSTRING_TO_CHAR(vstr);
    duk_push_string(ctx, vstrutf8);
    return 1;
}
static duk_ret_t dukpy_objwrap_get(duk_context *ctx) {
    // arguments: [wrappedObj key recv]
    duk_pop(ctx); // we don't care about recv

    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -2);

    PyObject* thing = NULL;

    switch (duk_get_type(ctx, -1)) {
        case DUK_TYPE_STRING:
        {
            const char* name = duk_require_string(ctx, -1);

            // is this toString?
            if (strncmp(name, "toString", 9) == 0) {
                // shortcircuit, return a callable
                duk_push_c_function(ctx, dukpy_objwrap_toString, 0);
                return 1;
            }
            if (PySequence_Check(v) && strncmp(name, "length", 7) == 0) {
                // shortcircuit, return the length
                int len = PySequence_Size(v);
                duk_push_int(ctx, len);
                return 1;
            }

            if (PyMapping_Check(v)) {
                thing = PyMapping_GetItemString(v, name);
            }

            if (thing == NULL) {
                PyErr_Clear();
                thing = PyObject_GetAttrString(v, name);
            }

            if (thing == NULL) {
                PyErr_Clear();
                return 0;
            }
        }
        break;

        case DUK_TYPE_NUMBER:
        {
            duk_int_t idx = duk_require_int(ctx, -1);

            if (PySequence_Check(v)) {
                thing = PySequence_GetItem(v, idx);
            }

            if (thing == NULL) {
                PyErr_Clear();
                return 0;
            }
        }
        break;
    }
    duk_pop_2(ctx);

    if (thing == NULL) {
        return 0;
    }

    return dukpy_wrap_a_python_object_somehow_and_return_it(ctx, thing);
}
static duk_ret_t dukpy_objwrap_set(duk_context *ctx) {
    // arguments: [wrappedObj key newVal recv]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -4);
    duk_pop(ctx); // don't want recv

    PyObject* seen = PyDict_New();
    PyObject* val = dukpy_pyobj_from_stack(ctx, -1, seen, 0, 0);
    Py_DECREF(seen);
    duk_pop(ctx);

    int status = -1;

    switch (duk_get_type(ctx, -1)) {
        case DUK_TYPE_STRING:
        {
            const char* name = duk_require_string(ctx, -1);

            // is this toString?
            if (strncmp(name, "toString", 9) == 0) {
                return 0;
            }
            if (PySequence_Check(v) && strncmp(name, "length", 7) == 0) {
                // TODO: make this not a noop and obey ECMAScript array semantics
                return 0;
            }

            if (PyMapping_Check(v)) {
                status = PyMapping_SetItemString(v, name, val);
            }

            if (status < 0) {
                PyErr_Clear();
                status = PyObject_SetAttrString(v, name, val);
            }

            if (status < 0) {
                PyErr_Clear();
                Py_DECREF(val);
                return 0;
            }
        }
        break;

        case DUK_TYPE_NUMBER:
        {
            duk_int_t idx = duk_require_int(ctx, -1);

            if (PySequence_Check(v)) {
                status = PySequence_SetItem(v, idx, val);
            }

            if (status < 0) {
                PyErr_Clear();
                Py_DECREF(val);
                return 0;
            }
        }
        break;
    }

    Py_DECREF(val);
    return 0;
}
static duk_ret_t dukpy_objwrap_has(duk_context *ctx) {
    // arguments: [wrappedObj key]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -2);

    int result = -1;

    switch (duk_get_type(ctx, -1)) {
        case DUK_TYPE_STRING:
        {
            const char* name = duk_require_string(ctx, -1);

            // is this toString?
            if (strncmp(name, "toString", 9) == 0) {
                result = 1;
            }
            // How about length?
            if (result < 1 && PySequence_Check(v) && strncmp(name, "length", 7) == 0) {
                result = 1;
            }

            if (result < 1 && PyMapping_Check(v)) {
                result = PyMapping_HasKeyString(v, name);
            }

            if (result < 1) {
                result = PyObject_HasAttrString(v, name);
            }
        }
        break;

        case DUK_TYPE_NUMBER:
        {
            duk_int_t idx = duk_require_int(ctx, -1);

            if (idx >= 0 && PySequence_Check(v)) {
                Py_ssize_t maxlen = PySequence_Size(v);
                result = maxlen > idx;
            }
        }
        break;
    }
    duk_pop_2(ctx);

    if (result < 1) {
        duk_push_false(ctx);
    } else {
        duk_push_true(ctx);
    }
    return 1;
}
static duk_ret_t dukpy_objwrap_deleteProperty(duk_context *ctx) {
    // arguments: [wrappedObj key]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -2);

    int status = -1;

    switch (duk_get_type(ctx, -1)) {
        case DUK_TYPE_STRING:
        {
            const char* name = duk_require_string(ctx, -1);

            // is this toString?
            if (strncmp(name, "toString", 9) == 0) {
                return 0;
            }
            if (PySequence_Check(v) && strncmp(name, "length", 7) == 0) {
                // TODO: make this not a noop and obey ECMAScript array semantics
                return 0;
            }

            if (PyMapping_Check(v)) {
                status = PyMapping_DelItemString(v, name);
            }

            if (status < 0) {
                PyErr_Clear();
                status = PyObject_DelAttrString(v, name);
            }

            if (status < 0) {
                PyErr_Clear();
                return 0;
            }
        }
        break;

        case DUK_TYPE_NUMBER:
        {
            duk_int_t idx = duk_require_int(ctx, -1);

            if (PySequence_Check(v)) {
                status = PySequence_DelItem(v, idx);
            }

            if (status < 0) {
                PyErr_Clear();
                return 0;
            }
        }
        break;
    }
    duk_pop_2(ctx);

    return 0;
}
static duk_ret_t dukpy_objwrap_enumerate_core(duk_context *ctx, int allowDir) {
    // arguments: [wrappedObj]
    PyObject* v = dukpy_get_objwrap_pyobj(ctx, -1);
    duk_pop(ctx);

    PyObject* itr = NULL;
    PyObject* item = NULL;
    int itemPos = 0;
    int usePositionAsKey = 0;
    int maskDunder = 0;
    int mustCoerceToString = 0;

    duk_push_array(ctx);

    // if we're a mapping, we return the keys
    if (PyMapping_Check(v)) {
        itr = PyMapping_Keys(v);
        if (itr == NULL) {
            PyErr_Clear();
        }
    }

    if (itr == NULL && (PySequence_Check(v) || DUKPY_IS_NSTRING(v))) {
        // If we're a sequence, we return the ints
        itr = PySequence_List(v);
        
        if (itr == NULL) {
            PyErr_Clear();
        } else {
            usePositionAsKey = 1;
        }
    }

    if (itr == NULL && allowDir) {
        // Otherwise, use dir
        itr = PyObject_Dir(v);
        
        if (itr == NULL) {
            PyErr_Clear();
        } else {
            maskDunder = 1;
        }
    }

    if (itr == NULL && !allowDir) {
        // We can try using the object as an iterator
        Py_INCREF(v);
        itr = v;
        mustCoerceToString = 1;
    }

    if (itr == NULL) {
        return 1;
    }

    PyObject* realIter = PyObject_GetIter(itr);
    if (realIter == NULL) {
        Py_DECREF(itr);
        return 1;
    }

    duk_push_number(ctx, 2);
    duk_put_prop_index(ctx, -2, 0);

    while ((item = PyIter_Next(realIter)) != NULL) {
        if (usePositionAsKey) {
            duk_push_number(ctx, itemPos);
            duk_put_prop_index(ctx, -2, itemPos);
        } else {
            if (mustCoerceToString) {
                PyObject* nitem = PyObject_Str(item);
                Py_DECREF(item);
                item = nitem;
            }

            if (!mustCoerceToString && PyNumber_Check(item)) {
                long val = PyLong_AsLong(item);
                if (!PyErr_Occurred()) {
                    duk_push_int(ctx, val);
                } else {
                    duk_push_undefined(ctx);
                }
            } else if (DUKPY_IS_NSTRING(item)) {
                char* val = DUKPY_NSTRING_TO_CHAR(item);
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
static duk_ret_t dukpy_objwrap_enumerate(duk_context *ctx) {
    return dukpy_objwrap_enumerate_core(ctx, 0);
}
static duk_ret_t dukpy_objwrap_ownKeys(duk_context *ctx) {
    return dukpy_objwrap_enumerate_core(ctx, 1);
}


static PyObject *DukPy_create_context(PyObject *self, PyObject *args) {
    PyObject *pyJSFunction;

    if (!PyArg_ParseTuple(args, "O", &pyJSFunction))
        return NULL;

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

    PyObject* pyctx = PyCapsule_New(ctx, DUKPY_CONTEXT_CAPSULE_NAME, &dukpy_destroy_pyctx);
    DUKPY_DEBUG_PRINT("pyctx is at %p, ctx is at %p\n", pyctx, ctx);
    duk_push_pointer(ctx, pyctx); // [gstash pyctx]
    duk_put_prop_string(ctx, -2, "pydukPyCTX"); // [gstash]

    Py_INCREF(pyJSFunction);
    duk_push_pointer(ctx, pyJSFunction); // [gstash pyJSFunction]
    duk_put_prop_string(ctx, -2, "pydukPyJSFunction"); // [gstash]
    DUKPY_DEBUG_PRINT("pyJSFunction is at %p\n", pyJSFunction);

    duk_pop(ctx); // []

    return pyctx;
}

static PyObject *DukPy_eval_string_ctx(PyObject *self, PyObject *args) {
    PyObject *pyctx;
    const char *command;
    PyObject *pyvars;

    if (!PyArg_ParseTuple(args, "OsO", &pyctx, &command, &pyvars))
        return NULL;

    duk_context *ctx = dukpy_ensure_valid_ctx(pyctx);
    if (!ctx) {
        PyErr_SetString(PyExc_ValueError, "must provide a duk_context");
        return NULL;
    }

    duk_gc(ctx, 0);

    if (dukpy_wrap_a_python_object_somehow_and_return_it(ctx, pyvars) != 1) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(DukPyError, "something went wrong");
        }
        return NULL;
    }
    duk_put_global_string(ctx, "dukpy");

    int res = duk_peval_string(ctx, command);
    if (res != 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(DukPyError, duk_safe_to_string(ctx, -1));
        }
        return NULL;
    }

    PyObject* seen = PyDict_New();
    PyObject* ret = dukpy_pyobj_from_stack(ctx, -1, seen, 0, 0);
    Py_DECREF(seen);
    duk_pop(ctx);

    return ret;
}

static PyObject *DukPy_add_global_object_ctx(PyObject *self, PyObject *args) {
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

    if (dukpy_wrap_a_python_object_somehow_and_return_it(ctx, object) != 1) {
        PyErr_SetString(PyExc_ValueError, ":(");
        return NULL;
    }
    duk_put_global_string(ctx, object_name); // []

    Py_RETURN_NONE;
}

static PyObject *DukPy_exec_dpf(PyObject *self, PyObject *args) {
    PyObject *pydpf;
    PyObject *pyarglist;

    if (!PyArg_ParseTuple(args, "OO", &pydpf, &pyarglist))
        return NULL;

    if (!pydpf) {
        PyErr_SetString(PyExc_ValueError, "must provide a PyDukFunction");
        return NULL;
    }

    if (!PyCapsule_CheckExact(pydpf)) {
        PyErr_SetString(PyExc_ValueError, "must provide a PyDukFunction");
        return NULL;
    }

    struct DukPyFunction* dpf = (struct DukPyFunction*)PyCapsule_GetPointer(pydpf, DUKPY_FUNCTION_CAPSULE_NAME);
    if (!dpf) {
        PyErr_SetString(PyExc_ValueError, "must provide a PyDukFunction");
        return NULL;
    }

    if (!pyarglist || !PySequence_Check(pyarglist)) {
        PyErr_SetString(PyExc_ValueError, "must provide an arglist");
        return NULL;
    }

    duk_push_global_stash(dpf->ctx); // [... gstash]
    duk_get_prop_string(dpf->ctx, -1, dpf->name); // [... gstash func]

    int argCount = dukpy_push_a_python_sequence_somehow_and_return_the_count(dpf->ctx, pyarglist);

    duk_pcall(dpf->ctx, argCount); // [... gstash res <args>]
    PyObject* seen = PyDict_New();
    PyObject* ret = dukpy_pyobj_from_stack(dpf->ctx, -1, seen, 0, 0);
    Py_DECREF(seen);
    duk_pop_2(dpf->ctx); // [...]

    return ret;
}

static PyMethodDef DukPy_methods[] = {
    {"new_context", DukPy_create_context, METH_VARARGS, "Create a new DukPy context."},
    {"ctx_eval_string", DukPy_eval_string_ctx, METH_VARARGS, "Run Javascript code from a string in a given context."},
    {"ctx_add_global_object", DukPy_add_global_object_ctx, METH_VARARGS, "Add an object to the global context."},
    {"dpf_exec", DukPy_exec_dpf, METH_VARARGS, "Execute a DukPyFunction."},
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
