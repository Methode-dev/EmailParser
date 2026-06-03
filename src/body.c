#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "body.h"
#include "headers.h"
#include <string.h>
#include <ctype.h>

const char *find_body_start(const char *text, size_t len) {
    /*
     * text: segment of the email
     * len: the length of the segment
     *
     * description:
     * iterating through the segment until a separator is found
     *
     * return: the body as a string
     */
    const char *p = text, *end = text + len;
    while (p < end && (*p == '\n' || *p == '\r'))
        p++;
    {
        const char *q = p;
        while (q < end && *q != '\n' && *q != ':')
            q++;
        if (q >= end || *q != ':')
            return text;
        const char *fname = p;
        size_t flen = (size_t)(q - p);
        while (flen > 0 && isspace((unsigned char)fname[flen - 1]))
            flen--;
        if (flen == 0)
            return text;
        for (size_t i = 0; i < flen; i++)
            if (isspace((unsigned char)fname[i]))
                return text;
        if (!canonical_key(fname, flen))
            return text;
    }
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n')
            p++;
        size_t llen = (size_t)(p - ls);
        if (llen > 0 && ls[llen - 1] == '\r')
            llen--;
        if (p < end)
            p++;
        if (llen == 0)
            return p;
    }
    return text;
}

PyObject *py_extract_body(PyObject *module, PyObject *args) {
    const char *text;
    Py_ssize_t text_len;
    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    const char *body = find_body_start(text, (size_t)text_len);
    return PyUnicode_DecodeUTF8(body, (Py_ssize_t)(text_len - (body - text)),
                                "replace");
}
