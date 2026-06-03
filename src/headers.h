#ifndef EMAILPARSER_HEADERS_H
#define EMAILPARSER_HEADERS_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stddef.h>

const char *canonical_key(const char *name, size_t len);
PyObject *py_parse_headers(PyObject *module, PyObject *args);
#endif
