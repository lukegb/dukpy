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

    def try_package_dir(self, folder_package):
        # grab package.json
        packagejson_path = os.path.join(folder_package, 'package.json')
        with open(packagejson_path) as f:
            packagejson = json.load(f)

        main_thing = packagejson.get('main')
        return os.path.join(folder_package, main_thing)

    def try_dir(self, path, id_):
        if os.path.exists(os.path.join(path, id_)):
            folder_package = os.path.join(path, id_)
            return self.try_package_dir(folder_package)

        if os.path.exists(os.path.join(path, id_ + '.js')):
            return os.path.join(path, id_ + '.js')

    def resolve(self, id_, search_paths):
        # we need to resolve id_ left-to-right
        search_paths = list(search_paths)
        id_segments = id_.split('!')
        last_found_path = None

        for id_segment in id_segments:
            next_search_paths = list(search_paths)
            for search_path in search_paths:
                ret = self.try_dir(search_path, id_segment)
                if not ret:
                    continue
                last_found_path = ret
                next_search_paths.insert(0, os.path.dirname(last_found_path))
                next_search_paths.insert(1, os.path.join(os.path.dirname(last_found_path), 'node_modules'))
                next_search_paths.insert(1, os.path.join(os.path.dirname(os.path.dirname(last_found_path)), 'node_modules'))
                break
            else:
                raise ImportError("unable to find " + id_)
            search_paths = next_search_paths

        with open(last_found_path, 'r') as f:
            return f.read()

    def require(self, req_ctx, id_, require, exports, module):
        # does the module ID begin with 'python/'
        if self.enable_python and id_.startswith('python/'):
            pyid = id_[len('python/'):].replace('/', '.')
            ret = self.require_python(pyid, require, exports, module)
            if ret:
                return ret

        ret = self.resolve(id_, self.search_paths)
        if ret:
            # do a slight cheat here
            ret = """
require = (function(_require) {
    return function(mToLoad) {
        var shouldPrependModuleID = false;

        var moduleSnippets = module.id.split('!');
        var lastModuleSnippet = moduleSnippets[moduleSnippets.length-1];

        if (lastModuleSnippet.indexOf('./') === 0) {
            // it's relative
            // in which case, we should only prepend if there's more than one /
            shouldPrependModuleID = lastModuleSnippet.lastIndexOf('/') !== 1;
        } else {
            // it's absolute
            shouldPrependModuleID = true;
        }

        var realModuleID = moduleSnippets.pop();

        if (shouldPrependModuleID) {
            mToLoad = module.id + '!' + mToLoad;
        } else {
            mToLoad = moduleSnippets.join('!') + '!' + mToLoad;
        }

        return _require(mToLoad);
    };
})(require);

//process = {'env': {'NODE_ENV': 'production'}};
(function() {
""" + ret + "})();"

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