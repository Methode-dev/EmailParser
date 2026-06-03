#ifndef EMAILPARSER_SIGNATURE_H
#define EMAILPARSER_SIGNATURE_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject *py_find_signature(PyObject *module, PyObject *args);
PyObject *py_strip_signature(PyObject *module, PyObject *args);
#endif
