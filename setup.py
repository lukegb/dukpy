import os
try:
    from setuptools import setup, Extension
except ImportError:
    from distutils.core import setup, Extension

HERE = os.path.abspath(os.path.dirname(__file__))
try:
    README = open(os.path.join(HERE, 'README.rst')).read()
except IOError:
    README = ''

duktape = Extension('dukpy._dukpy',
                    define_macros=[('DUK_OPT_DEEP_C_STACK', '1')],
                    extra_compile_args = ['-std=c99'],
                    sources=[os.path.join('duktape', 'duktape.c'), 
                             'pyduktape.c'],
                    include_dirs=[os.path.join('.', 'duktape')])

setup(
    name='dukpy-lukegb',
    version='0.0.2',
    description='Simple JavaScript interpreter for Python',
    long_description=README,
    keywords='javascript compiler babeljs coffeescript',
    author='Luke Granger-Brown & Alessandro Molina',
    author_email='python@lukegb.com',
    url='https://github.com/lukegb/dukpy',
    license='MIT',
    packages=['dukpy'],
    ext_modules=[duktape],
    package_data={
        'dukpy': ['*.js'],
    },
    classifiers=[
        'Development Status :: 3 - Alpha',
        'License :: OSI Approved :: MIT License',
        'Programming Language :: Python :: 3.5',
        'Programming Language :: Python :: 3.4',
        'Programming Language :: Python :: 3.3',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 2.6',
        'Programming Language :: JavaScript',
    ]
)
