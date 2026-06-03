#ifndef EMAILPARSER_STANDALONE_H
#define EMAILPARSER_STANDALONE_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stddef.h>

char *extract_css(const char *html, size_t html_len);
PyObject *wrap_standalone(const char *css, size_t css_len, const char *segment,
                          size_t seg_len);
#endif
