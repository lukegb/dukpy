import json
from . import _dukpy

try:  # pragma: no cover
    from collections.abc import Iterable
except ImportError:  # pragma: no cover
    from collections import Iterable

try:  # pragma: no cover
    unicode
    string_types = (str, unicode)
except NameError:  # pragma: no cover
    string_types = (bytes, str)


def evaljs(code, **kwargs):
    """Evaluates the given ``code`` as JavaScript and returns the result"""
    return Context().evaljs(code, **kwargs)


class JavaScriptCoercion(object):
    def __init__(self):
        self.asdf


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
        self._ctx = _dukpy.new_context()

    def _coerce_to(self, val):
        return json.dumps(val)

    def _coerce_from(self, res):
        if res is None:
            return None

        return json.loads(res.decode('utf-8'))

    def define_global_func(self, name, func):
        _dukpy.add_global_callable(self._ctx, name, func)

    def evaljs(self, code, **kwargs):
        """Evaluates the given ``code`` as JavaScript and returns the result"""
        jsvars = self._coerce_to(kwargs)
        jscode = code

        if not isinstance(code, string_types):
            jscode = ';\n'.join(code)

        return self._coerce_from(_dukpy.ctx_eval_string(self._ctx, jscode, jsvars))
