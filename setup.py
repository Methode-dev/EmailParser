import subprocess

from setuptools import Extension, setup


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
    name="fastemailparser",
    version="0.2.2",
    author="Julien Calenge @ Méthode",
    author_email="julien.calenge@methode.dev",
    description="Very fast email parsing tool, split emails, retrieve headers & signatures",
    long_description=open("README.md", "r").read(),
    long_description_content_type="text/markdown",
    url="https://github.com/Methode-dev/EmailParser",
    classifiers=[
        "Programming Language :: C",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
    ],
    ext_modules=[module],
)
