#ifndef EMAILPARSER_BODY_H
#define EMAILPARSER_BODY_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stddef.h>

const char *find_body_start(const char *text, size_t len);
PyObject *py_extract_body(PyObject *module, PyObject *args);
#endif
