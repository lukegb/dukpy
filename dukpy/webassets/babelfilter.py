# -*- coding: utf-8 -*-
from __future__ import absolute_import, print_function
from webassets.filter import Filter

import dukpy
import sys


__all__ = ('BabelJS', )


class BabelJS(Filter):
    name = 'babeljs'
    max_debug_level = None

    def input(self, _in, out, **kw):
        indata = _in.read()
        if sys.version_info[0] < 3:
            indata = indata.encode('utf-8')
        src = dukpy.babel_compile(indata)
        if sys.version_info[0] < 3:
            src = src.decode('utf-8')
        out.write(src)
