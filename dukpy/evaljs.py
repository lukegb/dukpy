from . import _dukpy

try:  # pragma: no cover
    unicode
    string_types = (str, unicode)
except NameError:  # pragma: no cover
    string_types = (bytes, str)


def evaljs(code, **kwargs):
    """Evaluates the given ``code`` as JavaScript and returns the result"""
    return Context().evaljs(code, **kwargs)


class ContextGlobals(object):
    def __init__(self, ctx):
        self._ctx = ctx

    def __getitem__(self, key):
        return self._ctx.global_get(key)

    def __setitem__(self, key, val):
        return self._ctx.global_set(key, val)

    def __delitem__(self, key):
        self._ctx.global_del(key)


class Context(object):
    def __init__(self):
        self._ctx = _dukpy.new_context(JSFunction)

    def define_global(self, name, obj):
        _dukpy.ctx_add_global_object(self._ctx, name, obj)

    def evaljs(self, code, **kwargs):
        """Evaluates the given ``code`` as JavaScript and returns the result"""
        jscode = code

        if not isinstance(code, string_types):
            jscode = ';\n'.join(code)

        return _dukpy.ctx_eval_string(self._ctx, jscode, kwargs)


class JSFunction(object):
    def __init__(self, func_ptr):
        self._func = func_ptr

    def __call__(self, *args):
        return _dukpy.dpf_exec(self._func, args)
