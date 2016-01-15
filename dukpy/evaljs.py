from . import _dukpy

import os.path
import importlib

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
        self._ctx = _dukpy.new_context(JSObject)

    def define_global(self, name, obj):
        _dukpy.ctx_add_global_object(self._ctx, name, obj)

    def evaljs(self, code, **kwargs):
        """Evaluates the given ``code`` as JavaScript and returns the result"""
        jscode = code

        if not isinstance(code, string_types):
            jscode = ';\n'.join(code)

        return _dukpy.ctx_eval_string(self._ctx, jscode, kwargs)


class RequirableContextFinder(object):
    def __init__(self, search_paths, enable_python=False):
        self.search_paths = search_paths
        self.enable_python = enable_python

    def contribute(self, req_ctx):
        req_ctx.evaljs("Duktape.modSearch = dukpy.modSearch;", modSearch=self.require)

    def require(self, id_, require, exports, module):
        # does the module ID begin with 'python/'
        if self.enable_python and id_.startswith('python/'):
            pyid = id_[len('python/'):].replace('/', '.')
            ret = self.require_python(pyid, require, exports, module)
            if ret:
                return ret

        for search_path in self.search_paths:
            # we'll assume here that search_path is relative to cwd
            # or is an absolute path(!)
            try:
                with open(os.path.join(search_path, id_ + '.js')) as f:
                    return f.read()
            except IOError:
                # assume this was probably No such file
                continue

        raise ImportError("Unable to load {}".format(id_))

    def require_python(self, pyid, require, exports, module):
        try:
            pymod = importlib.import_module(pyid)
        except ImportError:
            return None

        all_things = getattr(pymod, '__all__', [x for x in dir(pymod) if x and x[0] != '_'])
        for thing in all_things:
            exports[thing] = getattr(pymod, thing)
        return True


class RequirableContext(Context):
    def __init__(self, search_paths, enable_python=False):
        super(RequirableContext, self).__init__()
        self.finder = RequirableContextFinder(search_paths, enable_python)
        self.finder.contribute(self)


class JSObject(object):
    def __init__(self, ptr):
        self._ptr = ptr

    def __call__(self, *args):
        try:
            return _dukpy.dpf_exec(self._ptr, args)
        except _dukpy.JSRuntimeError as e:
            if str(e).startswith('TypeError: '):
                raise TypeError(str(e)[len('TypeError: '):])
            raise

    def __getitem__(self, key):
        return _dukpy.dpf_get_item(self._ptr, str(key))

    def __setitem__(self, key, value):
        return _dukpy.dpf_set_item(self._ptr, str(key), value)

    def __getattr__(self, key):
        if key == '_ptr':
            return super(JSObject, self).__getattr__(key)
        return self[key]

    def __setattr__(self, key, value):
        if key == '_ptr':
            return super(JSObject, self).__setattr__(key, value)
        self[key] = value

    def __len__(self):
        return self.length

    def __str__(self):
        return self.toString()