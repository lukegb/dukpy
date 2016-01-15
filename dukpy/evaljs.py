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
        req_ctx.evaljs("process = {}; process.env = dukpy.environ", environ=dict(os.environ))

    def try_package_dir(self, folder_package, id_):
        # grab package.json
        packagejson_path = os.path.join(folder_package, 'package.json')
        if os.path.exists(packagejson_path):
            with open(packagejson_path) as f:
                packagejson = json.load(f)

            main_thing = packagejson.get('main')
            if main_thing:
                return os.path.join(folder_package, main_thing), '{}/{}'.format(id_, main_thing)

        indexjs_path = os.path.join(folder_package, 'index.js')
        if os.path.exists(indexjs_path):
            return indexjs_path, '{}/index'.format(id_)

    def try_dir(self, path, id_):
        if id_.endswith('.json') and os.path.exists(os.path.join(path, id_)):
            return os.path.join(path, id_), id_

        if os.path.exists(os.path.join(path, id_)):
            folder_package = os.path.join(path, id_)
            return self.try_package_dir(folder_package, id_)

        if os.path.exists(os.path.join(path, id_ + '.js')):
            return os.path.join(path, id_ + '.js'), id_

        if os.path.exists(os.path.join(path, id_)):
            return os.path.join(path, id_), id_

    def resolve(self, id_, search_paths):
        # we need to resolve id_ left-to-right
        search_paths = list(search_paths)
        last_found_path = None
        canonical_id = id_

        for search_path in search_paths:
            r = self.try_dir(search_path, id_)
            if not r:
                continue
            ret, canonical_id = r
            last_found_path = ret
            break
        else:
            raise ImportError("unable to find " + id_)

        with open(last_found_path, 'r') as f:
            return f.read(), canonical_id

    def require(self, req_ctx, id_, require, exports, module):
        # does the module ID begin with 'python/'
        if self.enable_python and id_.startswith('python/'):
            pyid = id_[len('python/'):].replace('/', '.')
            ret = self.require_python(pyid, require, exports, module)
            if ret:
                return ret

        r = self.resolve(id_, self.search_paths)
        if r:
            ret, canonical_id = r
            if id_ and id_.endswith('.json'):
                return "module.exports = " + ret + ";"

            # do a slight cheat here
            # basically we want to make sure that the names
            # are resolved relative to the correct path, so we override
            # require.id to our "canonicalized" name
            #
            # we also need to make sure that <blah>/ requires are accepted
            req_ctx.define_global("_dukpy_last_module", canonical_id)
            ret = """
Object.defineProperty(require, "id", { value: _dukpy_last_module });
require = (function(_require) {
    return function(mToLoad) {
        if (mToLoad.charAt(mToLoad.length - 1) === '/') {
            // don't allow ending on an empty term
            mToLoad = mToLoad.slice(0, mToLoad.length - 1);
        }

        return _require(mToLoad);
    };
})(require);

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
