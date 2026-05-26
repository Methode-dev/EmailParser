#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <regex.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include "email.h"
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#ifndef SEPARATOR_REGEX
    #define SEPARATOR_REGEX_GEN_EN  "(From|Sent|To|Subject|Cc|Bcc) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX_GEN_FR  "(De|À|Envoyé|Objet|Cc|Cci) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX_STA_ALL "(De|From) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX_END_ALL "(Objet|Subject) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX         SEPARATOR_REGEX_STA_ALL
#endif

static int get_index_sep(char *body)
{
    regex_t    regex;
    regmatch_t match;

    if (regcomp(&regex, SEPARATOR_REGEX, REG_EXTENDED) != 0)
        return -1;
    int ret = regexec(&regex, body, 1, &match, 0);
    regfree(&regex);
    if (ret == REG_NOMATCH)
        return -1;
    return (int)match.rm_so;
}

static email_t *new_email(char *raw)
{
    email_t *e = malloc(sizeof(email_t));
    if (!e)
        return NULL;
    e->body       = raw;
    e->last_index = 0;
    return e;
}

static bool get_next_val(email_t *email)
{
    email->body += email->last_index;
    int idx = get_index_sep(email->body + 1);
    if (idx < 0)
        return false;
    email->last_index = idx + 1;  /* +1: idx is relative to body+1 */
    if (email->last_index >= 2)
        email->body[email->last_index - 2] = '\0';
    return true;
}

/* ── HTML → plain text ───────────────────────────────────────────────────── */

typedef struct { char *buf; size_t len, cap; } strbuf_t;

static int sb_push(strbuf_t *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = (sb->len + n + 1) * 2;
        char  *tmp = realloc(sb->buf, cap);
        if (!tmp) return -1;
        sb->buf = tmp;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static const char *BLOCK_TAGS[] = {
    "p","div","br","tr","li","h1","h2","h3","h4","h5","h6",NULL
};

static int is_block(const char *name)
{
    for (int i = 0; BLOCK_TAGS[i]; i++)
        if (!strcasecmp(name, BLOCK_TAGS[i])) return 1;
    return 0;
}

static int walk_text(xmlNodePtr node, strbuf_t *sb)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_TEXT_NODE && cur->content) {
            const char *s = (const char *)cur->content;
            if (sb_push(sb, s, strlen(s)) < 0) return -1;
        } else if (cur->type == XML_ELEMENT_NODE) {
            if (is_block((const char *)cur->name))
                if (sb_push(sb, "\n", 1) < 0) return -1;
            if (cur->children && walk_text(cur->children, sb) < 0)
                return -1;
        }
    }
    return 0;
}

static PyObject *segment_to_text(const char *html)
{
    htmlDocPtr doc = htmlReadMemory(html, (int)strlen(html), NULL, "UTF-8",
                                   HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc)
        return PyUnicode_FromString(html);   /* fallback: return as-is */

    strbuf_t   sb   = {NULL, 0, 0};
    xmlNodePtr root = xmlDocGetRootElement(doc);

    if (root && walk_text(root, &sb) < 0) {
        xmlFreeDoc(doc);
        free(sb.buf);
        PyErr_NoMemory();
        return NULL;
    }
    xmlFreeDoc(doc);

    PyObject *result = PyUnicode_FromStringAndSize(sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    return result;
}

/* ── Python type ─────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    email_t *email;
    char    *raw;
    int      exhausted;
    int      plain_text;
} EmailObject;

static void Email_dealloc(EmailObject *self)
{
    free(self->email);
    free(self->raw);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Email_init(EmailObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"source", "plain_text", NULL};

    /* support re-init */
    free(self->email); self->email = NULL;
    free(self->raw);   self->raw   = NULL;
    self->exhausted  = 0;
    self->plain_text = 0;

    PyObject *input;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p", kwlist,
                                     &input, &self->plain_text))
        return -1;

    char *raw  = NULL;
    long  size = 0;

    if (PyBytes_Check(input)) {
        Py_ssize_t len;
        char      *buf;
        if (PyBytes_AsStringAndSize(input, &buf, &len) < 0)
            return -1;
        size = (long)len;
        raw  = malloc(size + 1);
        if (!raw) { PyErr_NoMemory(); return -1; }
        memcpy(raw, buf, size);
        raw[size] = '\0';

    } else if (PyUnicode_Check(input)) {
        const char *s = PyUnicode_AsUTF8(input);
        if (!s) return -1;

        FILE *fd = fopen(s, "rb");
        if (fd) {
            fseek(fd, 0, SEEK_END);
            size = ftell(fd);
            rewind(fd);
            raw = malloc(size + 1);
            if (!raw) { fclose(fd); PyErr_NoMemory(); return -1; }
            raw[size] = '\0';
            fread(raw, size, 1, fd);
            fclose(fd);
        } else {
            /* treat the string itself as raw email content */
            size = (long)strlen(s);
            raw  = malloc(size + 1);
            if (!raw) { PyErr_NoMemory(); return -1; }
            memcpy(raw, s, size + 1);
        }

    } else {
        PyErr_SetString(PyExc_TypeError,
                        "expected str (file path or content) or bytes");
        return -1;
    }

    self->raw   = raw;
    self->email = new_email(raw);
    if (!self->email) { PyErr_NoMemory(); return -1; }
    return 0;
}

static PyObject *Email_iter(EmailObject *self)
{
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Email_next(EmailObject *self)
{
    if (self->exhausted)
        return NULL;
    if (!get_next_val(self->email)) {
        self->exhausted = 1;
        return NULL;
    }
    if (self->plain_text)
        return segment_to_text(self->email->body);
    return PyUnicode_FromString(self->email->body);
}

static PyTypeObject EmailType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "emailparser.Email",
    .tp_basicsize = sizeof(EmailObject),
    .tp_dealloc   = (destructor)Email_dealloc,
    .tp_iter      = (getiterfunc)Email_iter,
    .tp_iternext  = (iternextfunc)Email_next,
    .tp_init      = (initproc)Email_init,
    .tp_new       = PyType_GenericNew,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = PyDoc_STR(
        "Email(source, plain_text=False) -> iterator over quoted segments\n\n"
        "  source:     file path (str), raw content (str), or raw bytes\n"
        "  plain_text: if True, strip HTML tags and decode entities via libxml2\n\n"
        "Iterates over each reply segment split by email separator headers\n"
        "(e.g. 'From:', 'De:'). The separator regex can be overridden at\n"
        "compile time with -DSEPARATOR_REGEX='\"...\"'."
    ),
};

/* ── Signature detection ─────────────────────────────────────────────────── */

static const char *CLOSING_PATTERNS[] = {
    /* English */
    "best regards", "kind regards", "warm regards", "with regards",
    "many thanks", "best wishes", "yours sincerely", "yours faithfully",
    "yours truly", "sincerely", "cheers", "thanks", "regards",
    /* French */
    "cordialement", "bien cordialement", "merci", "salutations",
    NULL
};

/* Return 1 if the line (stripped of \r) is a signature delimiter or closing. */
static int is_signature_line(const char *line, size_t len)
{
    /* RFC 3676: "--" or "-- " on its own line */
    if (len >= 2 && line[0] == '-' && line[1] == '-') {
        if (len == 2 || (len == 3 && line[2] == ' '))
            return 1;
    }

    /* Formal closing: case-insensitive prefix match, optionally followed by
       punctuation/space so "regards" doesn't match "regardsomething". */
    char lower[64];
    size_t copy = len < 63 ? len : 63;
    for (size_t i = 0; i < copy; i++)
        lower[i] = (char)tolower((unsigned char)line[i]);
    lower[copy] = '\0';

    for (int i = 0; CLOSING_PATTERNS[i]; i++) {
        size_t plen = strlen(CLOSING_PATTERNS[i]);
        if (strncmp(lower, CLOSING_PATTERNS[i], plen) != 0)
            continue;
        /* must be end-of-line or followed by non-alpha (comma, space, …) */
        if (copy == plen || !isalpha((unsigned char)lower[plen]))
            return 1;
    }
    return 0;
}

/* Walk the DOM and return a malloc'd copy of the first text node whose
   (whitespace-stripped) content matches a signature pattern, or NULL. */
static char *find_sig_text_node(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_TEXT_NODE && cur->content) {
            const char *s = (const char *)cur->content;
            while (isspace((unsigned char)*s)) s++;        /* strip leading ws */
            size_t len = strlen(s);
            while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
            if (len > 0 && is_signature_line(s, len)) {
                char *copy = malloc(len + 1);
                if (!copy) return NULL;
                memcpy(copy, s, len);
                copy[len] = '\0';
                return copy;
            }
        }
        if (cur->children) {
            char *found = find_sig_text_node(cur->children);
            if (found) return found;
        }
    }
    return NULL;
}

/* Parse the HTML, find the first signature text node, then locate
   that literal text in the original HTML bytes. Returns byte offset or -1. */
static Py_ssize_t find_sig_in_html(const char *html, size_t html_len)
{
    htmlDocPtr doc = htmlReadMemory(html, (int)html_len, NULL, "UTF-8",
                                   HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return -1;

    xmlNodePtr root     = xmlDocGetRootElement(doc);
    char      *sig_text = root ? find_sig_text_node(root) : NULL;
    xmlFreeDoc(doc);

    if (!sig_text) return -1;

    const char *pos = memmem(html, html_len, sig_text, strlen(sig_text));
    Py_ssize_t  off = pos ? (Py_ssize_t)(pos - html) : -1;
    free(sig_text);
    return off;
}

/* Plain-text line scan — returns byte offset into text, or -1. */
static Py_ssize_t find_sig_in_plain(const char *text, size_t text_len)
{
    const char *p   = text;
    const char *end = text + text_len;

    while (p < end) {
        const char *line_start = p;
        while (p < end && *p != '\n') p++;

        size_t line_len  = (size_t)(p - line_start);
        size_t clean_len = (line_len > 0 && line_start[line_len - 1] == '\r')
                           ? line_len - 1 : line_len;

        if (is_signature_line(line_start, clean_len))
            return (Py_ssize_t)(line_start - text);

        if (p < end) p++;
    }
    return -1;
}

static PyObject *py_find_signature(PyObject *module, PyObject *args)
{
    const char *text;
    Py_ssize_t  text_len;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;

    Py_ssize_t byte_off = -1;

    /* Try HTML path if a '<' appears in the first 512 bytes */
    for (Py_ssize_t i = 0; i < text_len && i < 512; i++) {
        if (text[i] == '<') {
            byte_off = find_sig_in_html(text, (size_t)text_len);
            break;
        }
    }

    /* Fallback: plain-text line scan */
    if (byte_off < 0)
        byte_off = find_sig_in_plain(text, (size_t)text_len);

    if (byte_off < 0)
        return PyLong_FromLong(-1L);

    /* Convert byte offset → character offset for correct Python slicing */
    PyObject *prefix = PyUnicode_DecodeUTF8(text, byte_off, "replace");
    if (!prefix) return NULL;
    Py_ssize_t char_off = PyUnicode_GetLength(prefix);
    Py_DECREF(prefix);
    return PyLong_FromSsize_t(char_off);
}

/* ── Module ──────────────────────────────────────────────────────────────── */

static PyMethodDef emailparser_methods[] = {
    {"find_signature", py_find_signature, METH_VARARGS,
     PyDoc_STR(
         "find_signature(text) -> int\n\n"
         "Return the character index where the signature starts, or -1.\n\n"
         "Detects:\n"
         "  - RFC 3676 delimiter: a line containing exactly '--' or '-- '\n"
         "  - Common English closings: 'Best regards', 'Kind regards', 'Sincerely', …\n"
         "  - Common French closings: 'Cordialement', 'Bien cordialement', …\n\n"
         "Accepts both plain text and raw HTML segments.\n"
         "For HTML, the signature is located via libxml2 DOM parsing;\n"
         "plain-text line scanning is used as a fallback."
     )},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef emailparser_module = {
    PyModuleDef_HEAD_INIT,
    .m_name    = "emailparser",
    .m_doc     = PyDoc_STR("C extension for parsing email reply chains."),
    .m_size    = -1,
    .m_methods = emailparser_methods,
};

PyMODINIT_FUNC PyInit_emailparser(void)
{
    if (PyType_Ready(&EmailType) < 0)
        return NULL;

    PyObject *m = PyModule_Create(&emailparser_module);
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
