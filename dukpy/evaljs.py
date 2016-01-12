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


class Context(object):
    def __init__(self):
        self._ctx = _dukpy.new_context()

    def evaljs(self, code, **kwargs):
        """Evaluates the given ``code`` as JavaScript and returns the result"""
        jsvars = json.dumps(kwargs)
        jscode = code

        if not isinstance(code, string_types):
            jscode = ';\n'.join(code)

        res = _dukpy.ctx_eval_string(self._ctx, jscode, jsvars)
        if res is None:
            return None

        return json.loads(res.decode('utf-8'))
