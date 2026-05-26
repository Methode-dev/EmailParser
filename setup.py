import subprocess
from setuptools import setup, Extension

def xml2_config(flag):
    return subprocess.check_output(["xml2-config", flag]).decode().split()

module = Extension(
    "emailparser",
    sources=["emailparser.c"],
    extra_compile_args=xml2_config("--cflags"),
    extra_link_args=xml2_config("--libs"),
)

setup(
    name="emailparser",
    version="0.1.0",
    ext_modules=[module],
)
