from distutils.core import setup, Extension

MAJOR = 0
MINOR = 0
MICRO = 0
VERSION = '{}.{}.{}'.format(MAJOR, MINOR, MICRO)

with open('README.md', 'r') as f:
    long_description = f.read()

module_asyncloader = Extension(
    'AsyncLoader',
    sources = [
        'csrc/asyncmodule/asyncmodule.c',
        'csrc/async/async.c',
        'csrc/utils/alloc.c'
    ],
    extra_link_args = [
        '-lpthread',
        '-luring'
    ],
)

setup(name = 'Async File Loader',
      version = VERSION,
      description = 'Python asynchronous file loader module',
      long_description = long_description,
      long_description_content_type = 'text/markdown',
      platforms = "any",
      author = 'Gus Waldspurger',
      author_email = 'gus@waldspurger.com',
      license = 'MIT',
      ext_modules = [module_asyncloader])