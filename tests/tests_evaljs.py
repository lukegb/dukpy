import json
import os.path
import dukpy
from diffreport import report_diff


class TestEvalJS(object):
    def test_object_return(self):
        ans = dukpy.evaljs(["var o = {'value': 5}",
                            "o['value'] += 3",
                            "o"])
        assert ans == {'value': 8}

    def test_coffee(self):
        ans = dukpy.coffee_compile('''
    fill = (container, liquid = "coffee") ->
        "Filling the #{container} with #{liquid}..."
''')
        assert ans == '''(function() {
  var fill;

  fill = function(container, liquid) {
    if (liquid == null) {
      liquid = "coffee";
    }
    return "Filling the " + container + " with " + liquid + "...";
  };

}).call(this);
'''

    def test_sum(self):
        n = dukpy.evaljs("dukpy['value'] + 3", value=7)
        assert n == 10

    def test_babel(self):
        ans = dukpy.babel_compile('''
class Point {
    constructor(x, y) {
        this.x = x;
        this.y = y;
    }
    toString() {
        return '(' + this.x + ', ' + this.y + ')';
    }
}
''')
        assert '''var Point = (function () {
    function Point(x, y) {
''' in ans, ans

    def test_typescript(self):
        ans = dukpy.typescript_compile('''
class Greeter {
    constructor(public greeting: string) { }
    greet() {
        return "<h1>" + this.greeting + "</h1>";
    }
};

var greeter = new Greeter("Hello, world!");
''')

        expected = """var Greeter = (function () {
    function Greeter(greeting) {
        this.greeting = greeting;
    }
    Greeter.prototype.greet = function () {
        return "<h1>" + this.greeting + "</h1>";
    };
    return Greeter;
})();
;
var greeter = new Greeter("Hello, world!");"""

        assert expected in ans, report_diff(expected, ans)


class TestContext(object):
    def test_can_construct_context(self):
        dukpy.Context()

    def test_evaljs(self):
        c = dukpy.Context()
        ret = c.evaljs("5")
        assert ret == 5

    def test_context_retained(self):
        c = dukpy.Context()
        c.evaljs("aardvark = 'lemon'")
        assert c.evaljs("aardvark") == 'lemon'

    def test_handles_none(self):
        o = dukpy._dukpy.ctx_eval_string
        dukpy._dukpy.ctx_eval_string = lambda *args, **kwargs: None
        c = dukpy.Context()
        c.evaljs("hi")
        dukpy._dukpy.ctx_eval_string = o


class TestPythonToJSObject(object):
    def test_can_set_obj(self):
        c = dukpy.Context()
        c.define_global("seven", 7)
        assert c.evaljs("seven") == 7

    def test_can_pass_callable(self):
        c = dukpy.Context()
        called = {'called': False}

        def inner():
            called['called'] = True

        c.define_global("func", inner)
        c.evaljs("func()")
        assert called['called']

    def test_can_pass_dict(self):
        c = dukpy.Context()
        d = {'worked': False}
        c.define_global("dict", d)
        c.evaljs("dict.worked = true")
        assert d['worked']

    def test_can_pass_class(self):
        c = dukpy.Context()

        class Test(object):
            def __init__(self, worked):
                self.worked = worked

        c.define_global("Test", Test)
        t = c.evaljs("Test(true)")
        assert t.worked

    def test_can_pass_obj(self):
        c = dukpy.Context()

        class Test(object):
            def __init__(self):
                self.worked = False

        t = Test()
        c.define_global("t", t)
        c.evaljs("t.worked = true")
        assert t.worked

    def test_can_return_callable(self):
        c = dukpy.Context()

        inc = c.evaljs("(function(x) { return x + 1; })")
        assert inc(1) == 2
        assert inc(2) == 3

    def test_can_return_obj(self):
        c = dukpy.Context()

        obj = c.evaljs("({'hi': 'mum'})")
        assert obj['hi'] == 'mum'

    def test_can_return_complex_obj(self):
        c = dukpy.Context()

        obj = c.evaljs("""
            ({'null': null, 'undefined': null, 'a': 'a', '1': 1, 'true': true, 'false': false, 'nested': {'object': true}, 'array': ['of', ['arrays'], {'and': 'objects'}]})
        """)
        assert obj['null'] is None
        assert obj['undefined'] is None
        assert obj['a'] == 'a'
        assert obj['1'] == 1
        assert obj['true'] is True
        assert obj['false'] is False
        assert obj['nested']['object']
        assert len(obj['array']) == 3
        assert obj['array'][0] == 'of'
        assert len(obj['array'][1]) == 1
        assert obj['array'][1][0] == 'arrays'
        assert obj['array'][2]['and'] == 'objects'

    def test_can_iterate_over_python(self):
        c = dukpy.Context()

        c.define_global("thing", [5, 8, 1, 6])
        ret = c.evaljs("""
            var count = 0;
            for (var i = 0; i < thing.length; i++) {
                count += thing[i];
            }
            count;
        """)
        assert ret == sum([5, 8, 1, 6])

    def test_tostring_calls_str(self):
        c = dukpy.Context()

        class Test(object):
            def __init__(self, str):
                self.s = str

            def __str__(self):
                return self.s

        c.define_global("t", Test('hi, mum'))
        ret = c.evaljs("t.toString()")
        assert ret == "hi, mum"

    def test_complex_pass(self):
        c = dukpy.Context()

        def call_first_argument_python(a, *args):
            return a(*args)

        c.define_global("call_first_argument_python", call_first_argument_python)
        call_first_argument_js = c.evaljs("(function(a) { return a.apply(undefined, arguments); })")

        ret = call_first_argument_js(call_first_argument_python, sum, [1, 7, 10])
        assert ret == 18


class TestRequirableContext(object):
    test_js_dir = os.path.join(os.path.dirname(__file__), 'testjs')

    def test_js_require(self):
        c = dukpy.RequirableContext([self.test_js_dir])
        assert c.evaljs("require('testjs').call()") == "Hello from JS!"

    def test_python_require(self):
        import sys, imp
        testpy = imp.new_module('testpy')
        testpy.call = lambda: 'Hello from Python!'
        testpy.__all__ = ['call']
        sys.modules['testpy'] = testpy
        c = dukpy.RequirableContext([], enable_python=True)
        assert c.evaljs("require('python/testpy').call()") == "Hello from Python!"


class TestRegressions(object):
    def test_pyargs_segfault(self):
        for _ in range(1, 10):
            c = dukpy.Context()
            c.define_global("test", range(1000))
            c.evaljs("""
var iter = test.__iter__();
var i = (iter.__next__ || iter.next)();
while (i !== null) {
    i = (iter.__next__ || iter.next)();
    break;
}
            """)
