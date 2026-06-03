import subprocess
from setuptools import setup, Extension

def xml2_config(flag):
    return subprocess.check_output(["xml2-config", flag]).decode().split()

src = [
    "emailparser.c",
    "src/mime.c",
    "src/html.c",
    "src/standalone.c",
    "src/email_iter.c",
    "src/headers.c",
    "src/body.c",
    "src/signature.c",
]

module = Extension(
    "emailparser",
    sources=src,
    include_dirs=[".", "src"],
    extra_compile_args=xml2_config("--cflags"),
    extra_link_args=xml2_config("--libs"),
)

setup(
    name="emailparser",
    version="0.1.0",
    ext_modules=[module],
)
