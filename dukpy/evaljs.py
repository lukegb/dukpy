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
        self._ctx = _dukpy.new_context(JSFunction)

    def define_global(self, name, obj):
        _dukpy.ctx_add_global_object(self._ctx, name, obj)

    def evaljs(self, code, **kwargs):
        """Evaluates the given ``code`` as JavaScript and returns the result"""
        jscode = code

        if not isinstance(code, string_types):
            jscode = ';\n'.join(code)

        return _dukpy.ctx_eval_string(self._ctx, jscode, kwargs)


class RequirableContext(Context):
    def __init__(self, search_paths, enable_python=False):
        super(RequirableContext, self).__init__()
        self.search_paths = search_paths
        self.evaljs("Duktape.modSearch = dukpy.modSearch;", modSearch=self.require)
        self.enable_python = True

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

        raise Exception("Unable to load {}".format(id_))

    def require_python(self, pyid, require, exports, module):
        try:
            pymod = importlib.import_module(pyid)
        except ImportError:
            return None

        # hack to work around the fact that
        # JS objects in Python are copies, and don't reflect the state of the object
        # "in JS land"
        all_things = getattr(pymod, '__all__', [x for x in dir(pymod) if x and x[0] != '_'])
        self.evaljs("""
Duktape._pymodules = Duktape._pymodules || {};
Duktape._pymodulesins = Duktape._pymodulesins || {};

Duktape._pylastmodule = dukpy.module_name;
Duktape._pymodules[dukpy.module_name] = dukpy.module;
Duktape._pymodulesins[dukpy.module_name] = dukpy.module_things;
""", module_name=pyid, module=pymod, module_things=all_things)
        return """
var a = Duktape._pymodulesins[Duktape._pylastmodule];
for (var i = 0; i < a.length; i++) {
    var k = a[i];
    exports[k] = Duktape._pymodules[Duktape._pylastmodule][k];
}
"""


class JSFunction(object):
    def __init__(self, func_ptr):
        self._func = func_ptr

    def __call__(self, *args):
        return _dukpy.dpf_exec(self._func, args)
