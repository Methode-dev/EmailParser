#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "email.h"
#include "src/email_iter.h"
#include "src/mime.h"
#include "src/html.h"
#include "src/standalone.h"
#include "src/headers.h"
#include "src/body.h"
#include "src/signature.h"

typedef struct {
    PyObject_HEAD email_t *email;
    char *raw;
    char *css;
    size_t css_len;
    size_t outer_hdr_len;
    int exhausted;
    int plain_text;
    int standalone;
    int strip_headers;
} EmailObject;

static void Email_dealloc(EmailObject *self) {
    /*
     * self: the Email iterator object being destroyed
     *
     * description:
     * frees all heap-allocated fields owned by the object before
     * delegating to the type's tp_free slot.
     *
     * return: nothing
     */
    free(self->email);
    free(self->raw);
    free(self->css);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Email_init(EmailObject *self, PyObject *args, PyObject *kwds) {
    /*
     * self: the Email iterator object being initialised
     * args: positional arguments (source, …)
     * kwds: keyword arguments (plain_text, standalone, strip_headers)
     *
     * description:
     * reads source as a file path, raw string, or bytes; allocates a
     * buffer; creates an email_t iterator; optionally skips outer MIME
     * headers; and extracts CSS for standalone mode.
     * supports re-initialisation by freeing previous state first.
     *
     * return: 0 on success, -1 on error (Python exception is set)
     */
    static char *kwlist[] = {"source", "plain_text", "standalone",
                             "strip_headers", NULL};
    PyObject *input;
    char *raw;
    long size;
    Py_ssize_t blen;
    char *buf;
    const char *s;
    FILE *fd;
    char *body;

    free(self->email);
    self->email = NULL;
    free(self->raw);
    self->raw = NULL;
    free(self->css);
    self->css = NULL;
    self->css_len = 0;
    self->outer_hdr_len = 0;
    self->exhausted = 0;
    self->plain_text = 0;
    self->standalone = 0;
    self->strip_headers = 0;
    raw = NULL;
    size = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ppp", kwlist, &input,
                                     &self->plain_text, &self->standalone,
                                     &self->strip_headers))
        return -1;
    if (PyBytes_Check(input)) {
        if (PyBytes_AsStringAndSize(input, &buf, &blen) < 0)
            return -1;
        size = (long)blen;
        raw = malloc(size + 1);
        if (!raw) {
            PyErr_NoMemory();
            return -1;
        }
        memcpy(raw, buf, size);
        raw[size] = '\0';
    } else if (PyUnicode_Check(input)) {
        s = PyUnicode_AsUTF8(input);
        if (!s)
            return -1;
        fd = fopen(s, "rb");
        if (fd) {
            fseek(fd, 0, SEEK_END);
            size = ftell(fd);
            rewind(fd);
            raw = malloc(size + 1);
            if (!raw) {
                fclose(fd);
                PyErr_NoMemory();
                return -1;
            }
            raw[size] = '\0';
            fread(raw, size, 1, fd);
            fclose(fd);
        } else {
            size = (long)strlen(s);
            raw = malloc(size + 1);
            if (!raw) {
                PyErr_NoMemory();
                return -1;
            }
            memcpy(raw, s, size + 1);
        }
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "expected str (file path or content) or bytes");
        return -1;
    }
    self->raw = raw;
    self->email = new_email(raw);
    if (!self->email) {
        PyErr_NoMemory();
        return -1;
    }
    body = skip_mime_headers(raw);
    if (body != raw) {
        self->email->body = body;
        self->email->yield_if_empty_chain = 1;
        self->outer_hdr_len = (size_t)(body - raw);
    }
    if (self->standalone && !self->plain_text) {
        self->css = extract_css(raw, (size_t)size);
        self->css_len = self->css ? strlen(self->css) : 0;
    }
    return 0;
}

static PyObject *Email_iter(EmailObject *self) {
    /*
     * self: the Email iterator object
     *
     * description:
     * returns self to satisfy the iterator protocol; increments the
     * reference count so the object stays alive while iteration is
     * in progress.
     *
     * return: self with an incremented reference count
     */
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Email_next(EmailObject *self) {
    /*
     * self: the Email iterator object
     *
     * description:
     * advances the internal email_t state to the next segment and
     * returns it as a Python string. applies strip_headers, plain_text,
     * or standalone post-processing as requested. raises StopIteration
     * implicitly by returning NULL without setting an exception.
     *
     * return: Python string for the next segment, or NULL when exhausted
     */
    const char *body;
    const char *css;

    if (self->exhausted)
        return NULL;
    if (!get_next_val(self->email)) {
        self->exhausted = 1;
        return NULL;
    }
    body = self->email->body;
    if (self->strip_headers)
        body = find_body_start(body, strlen(body));
    if (self->plain_text)
        return segment_to_text(body);
    if (self->standalone) {
        css = self->css ? self->css : "";
        return wrap_standalone(css, self->css_len, body, strlen(body));
    }
    return PyUnicode_FromString(body);
}

static PyObject *Email_get_outer_headers(EmailObject *self, void *closure) {
    /*
     * self: the Email iterator object
     * closure: unused
     *
     * description:
     * exposes the outer MIME header block (the metadata of the most
     * recent email) as a parsed dict, identical in structure to what
     * parse_headers() returns. only available for raw MIME emails;
     * returns None for pure HTML emails.
     *
     * return: Python dict from parse_headers, or None
     */
    PyObject *arg;
    PyObject *result;

    if (self->outer_hdr_len == 0)
        Py_RETURN_NONE;
    arg = Py_BuildValue("(s#)", self->raw, (Py_ssize_t)self->outer_hdr_len);
    if (!arg)
        return NULL;
    result = py_parse_headers(NULL, arg);
    Py_DECREF(arg);
    return result;
}

static PyGetSetDef Email_getset[] = {
    {"outer_headers", (getter)Email_get_outer_headers, NULL,
     PyDoc_STR("Parsed headers of the most recent email "
               "(From, To, CC, Subject, Date)\n"
               "extracted from the outer MIME header block.\n"
               "Returns None for pure HTML emails that have no outer "
               "MIME headers."),
     NULL},
    {NULL}};

static PyTypeObject EmailType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "emailparser.Email",
    .tp_basicsize = sizeof(EmailObject),
    .tp_dealloc = (destructor)Email_dealloc,
    .tp_iter = (getiterfunc)Email_iter,
    .tp_iternext = (iternextfunc)Email_next,
    .tp_init = (initproc)Email_init,
    .tp_new = PyType_GenericNew,
    .tp_getset = Email_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = PyDoc_STR(
        "Email(source, plain_text=False, standalone=False, "
        "strip_headers=False)\n\n"
        "  source:        file path (str), raw content (str), or raw bytes\n"
        "  plain_text:    strip HTML tags and decode entities via libxml2\n"
        "  standalone:    wrap each HTML segment in a full document with\n"
        "                 extracted <style> CSS. Ignored when "
        "plain_text=True.\n"
        "  strip_headers: remove the From/To/Subject/Date header block from\n"
        "                 the top of each quoted segment, leaving only the "
        "body.\n\n"
        "Iterates over each reply segment split by email separator headers\n"
        "(e.g. 'From:', 'De:'). Override the separator regex at compile "
        "time\nwith -DSEPARATOR_REGEX='\"...\"'."),
};

static PyMethodDef emailparser_methods[] = {
    {"strip_signature", py_strip_signature, METH_VARARGS,
     PyDoc_STR("strip_signature(text) -> str\n\n"
               "Return text with the signature block removed.")},
    {"extract_body", py_extract_body, METH_VARARGS,
     PyDoc_STR("extract_body(segment) -> str\n\n"
               "Return the body, stripping the header block from the top.")},
    {"parse_headers", py_parse_headers, METH_VARARGS,
     PyDoc_STR("parse_headers(segment) -> dict\n\n"
               "Extract From/To/CC/BCC/Subject/Date from a segment.")},
    {"find_signature", py_find_signature, METH_VARARGS,
     PyDoc_STR("find_signature(text) -> int\n\n"
               "Return the char index where the signature starts, or -1.")},
    {NULL, NULL, 0, NULL}};

static PyModuleDef emailparser_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "emailparser",
    .m_doc = PyDoc_STR("C extension for parsing email reply chains."),
    .m_size = -1,
    .m_methods = emailparser_methods,
};

PyMODINIT_FUNC PyInit_emailparser(void) {
    /*
     * description:
     * registers the Email type and creates the emailparser module,
     * exposing parse_headers, extract_body, find_signature and
     * strip_signature as module-level functions.
     *
     * return: new module object, or NULL on failure
     */
    PyObject *m;

    if (PyType_Ready(&EmailType) < 0)
        return NULL;
    m = PyModule_Create(&emailparser_module);
    if (!m)
        return NULL;
    Py_INCREF(&EmailType);
    if (PyModule_AddObject(m, "Email", (PyObject *)&EmailType) < 0) {
        Py_DECREF(&EmailType);
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
