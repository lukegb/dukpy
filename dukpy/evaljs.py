from . import _dukpy

import os.path
import json
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
        import os
        req_ctx.evaljs("Duktape.modSearch = dukpy.modSearch;", modSearch=req_ctx.wrap(self.require))
        req_ctx.evaljs("Duktape.resolverBase = null;")
        req_ctx.evaljs("process = {}; process.env = dukpy.environ", environ=dict(os.environ))

    def load_node_modules(self, search_id, search_path, recurse_downwards):
        if recurse_downwards:
            node_modules_dirs = []

            search_path_pieces = search_path.split('/')
            for n in range(len(search_path_pieces)-1, -1, -1):
                if search_path_pieces[n] == 'node_modules':
                    continue  # we'll end up generating this anyway
                node_modules_dirs.append(os.path.join('/'.join(search_path_pieces[:n]), 'node_modules'))
        else:
            node_modules_dirs = [search_path]

        for node_modules_dir in node_modules_dirs:
            ret = self.load_as_file_or_directory(os.path.join(node_modules_dir, search_id))
            if ret:
                return ret

    def load_as_file_or_directory(self, path):
        try_files = [path, path + '.js', path + '.json']
        for try_file in try_files:
            if os.path.exists(try_file) and os.path.isfile(try_file):
                return try_file

        packagejson = os.path.join(path, 'package.json')
        if os.path.exists(packagejson):
            try:
                with open(packagejson) as f:
                    package = json.load(f)
                if 'main' in package:
                    ret = self.load_as_file_or_directory(os.path.join(path, package['main']))
                    if ret:
                        return ret
            except IOError:
                pass

        nextsteps = ['index.js', 'index.json']
        for nextstep in nextsteps:
            nextstep = os.path.join(path, nextstep)
            if os.path.exists(nextstep):
                return nextstep


    def resolve(self, req_ctx, id_, search_paths):
        # we need to resolve id_ left-to-right
        search_paths = list(search_paths)
        if id_[0] == '!' and id_[-1] == '!':
            start_path = str(req_ctx.evaljs("Duktape.resolverBase"))
            search_id = id_[1:-1]
        else:
            start_path = None
            search_id = id_
        found_path = None

        if not found_path and start_path:
            found_path = self.load_as_file_or_directory(os.path.join(start_path, search_id))

        if not found_path and start_path:
            found_path = self.load_node_modules(search_id, start_path, True)

        if not found_path:
            for search_path in search_paths:
                found_path = self.load_node_modules(search_id, search_path, False)

        if not found_path:
            raise ImportError("unable to find " + id_)

        with open(found_path, 'r') as f:
            return f.read(), found_path

    def require(self, req_ctx, id_, require, exports, module):
        # does the module ID begin with 'python/'
        if self.enable_python and id_.startswith('python/'):
            pyid = id_[len('python/'):].replace('/', '.')
            ret = self.require_python(pyid, require, exports, module)
            if ret:
                return ret

        r = self.resolve(req_ctx, id_, self.search_paths)
        if r:
            ret, located_path = r
            if located_path.endswith('.json'):
                return "module.exports = (" + ret + ");"

            # do a slight cheat here
            # basically we want to make sure that the names
            # are resolved relative to the correct path, so we override
            # require.id to our "canonicalized" name
            #
            # we also need to make sure that <blah>/ requires are accepted
            req_ctx.define_global("_dukpy_last_module", os.path.dirname(located_path))
            ret = """
require = (function(_require, locatedPath) {
    return function(mToLoad) {
        Duktape.resolverBase = locatedPath;
        return _require('!' + mToLoad + '!');
    };
})(require, _dukpy_last_module);
delete _dukpy_last_module;

(function() {
""" + ret + """
})();
"""

            return ret

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

    def wrap(self, callable):
        def inner(*args, **kwargs):
            return callable(self, *args, **kwargs)
        return inner


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
        try:
            return self.toString()
        except:
            return super(JSObject, self).__str__()
