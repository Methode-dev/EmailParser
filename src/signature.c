#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "signature.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

static const char *CLOSING_PATTERNS[] = {
    "best regards",    "kind regards",     "warm regards",
    "with regards",    "many thanks",      "best wishes",
    "yours sincerely", "yours faithfully", "yours truly",
    "sincerely",       "cheers",           "thanks",
    "regards",         "cordialement",     "bien cordialement",
    "merci",           "salutations",      NULL};

static int is_signature_line(const char *line, size_t len) {
    /*
     * line: one line of text (without trailing \n)
     * len: byte length of line
     *
     * description:
     * returns 1 if line is an RFC 3676 "--" delimiter or starts with
     * one of the known English/French formal closing phrases.
     * the match is case-insensitive and requires a non-alpha character
     * (comma, space …) or end-of-string after the closing phrase so
     * that "regardsomething" does not match.
     *
     * return: 1 if line is a signature line, 0 otherwise
     */
    char lower[64];
    size_t copy;
    size_t plen;
    int i;

    if (len >= 2 && line[0] == '-' && line[1] == '-')
        if (len == 2 || (len == 3 && line[2] == ' '))
            return 1;
    copy = len < 63 ? len : 63;
    for (size_t j = 0; j < copy; j++)
        lower[j] = (char)tolower((unsigned char)line[j]);
    lower[copy] = '\0';
    for (i = 0; CLOSING_PATTERNS[i]; i++) {
        plen = strlen(CLOSING_PATTERNS[i]);
        if (strncmp(lower, CLOSING_PATTERNS[i], plen) != 0)
            continue;
        if (copy == plen || !isalpha((unsigned char)lower[plen]))
            return 1;
    }
    return 0;
}

static char *find_sig_text_node(xmlNodePtr node) {
    /*
     * node: root of the XML/HTML subtree to search
     *
     * description:
     * walks the DOM looking for the first text node whose trimmed
     * content matches is_signature_line. returns a malloc'd copy of
     * that content, or NULL if none is found.
     *
     * return: malloc'd NUL-terminated signature text, or NULL; caller frees
     */
    xmlNodePtr cur;
    const char *s;
    size_t len;
    char *copy;

    for (cur = node; cur; cur = cur->next) {
        if (cur->type == XML_TEXT_NODE && cur->content) {
            s = (const char *)cur->content;
            while (isspace((unsigned char)*s))
                s++;
            len = strlen(s);
            while (len > 0 && isspace((unsigned char)s[len - 1]))
                len--;
            if (len > 0 && is_signature_line(s, len)) {
                copy = malloc(len + 1);
                if (!copy)
                    return NULL;
                memcpy(copy, s, len);
                copy[len] = '\0';
                return copy;
            }
        }
        if (cur->children) {
            copy = find_sig_text_node(cur->children);
            if (copy)
                return copy;
        }
    }
    return NULL;
}

static Py_ssize_t find_sig_in_html(const char *html, size_t html_len) {
    /*
     * html: raw HTML segment to search
     * html_len: byte length of html
     *
     * description:
     * parses the HTML with libxml2, walks the DOM to find the first
     * signature text node, then locates that literal string in the
     * original HTML bytes with memmem.
     *
     * return: byte offset of signature start in html, or -1 if not found
     */
    htmlDocPtr doc;
    xmlNodePtr root;
    char *sig_text;
    const char *pos;
    Py_ssize_t off;

    doc = htmlReadMemory(html, (int)html_len, NULL, "UTF-8",
                         HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc)
        return -1;
    root = xmlDocGetRootElement(doc);
    sig_text = root ? find_sig_text_node(root) : NULL;
    xmlFreeDoc(doc);
    if (!sig_text)
        return -1;
    pos = memmem(html, html_len, sig_text, strlen(sig_text));
    off = pos ? (Py_ssize_t)(pos - html) : -1;
    free(sig_text);
    return off;
}

static Py_ssize_t find_sig_in_plain(const char *text, size_t text_len) {
    /*
     * text: plain-text segment to search
     * text_len: byte length of text
     *
     * description:
     * scans text line by line, stripping trailing \r, and calls
     * is_signature_line on each. returns the byte offset of the
     * first matching line.
     *
     * return: byte offset of signature start, or -1 if not found
     */
    const char *p;
    const char *end;
    const char *ls;
    size_t llen;
    size_t clean;

    p = text;
    end = text + text_len;
    while (p < end) {
        ls = p;
        while (p < end && *p != '\n')
            p++;
        llen = (size_t)(p - ls);
        clean = (llen > 0 && ls[llen - 1] == '\r') ? llen - 1 : llen;
        if (is_signature_line(ls, clean))
            return (Py_ssize_t)(ls - text);
        if (p < end)
            p++;
    }
    return -1;
}

PyObject *py_find_signature(PyObject *module, PyObject *args) {
    /*
     * module: unused Python module argument
     * args: Python tuple containing one string segment
     *
     * description:
     * tries the HTML path first (DOM parsing via libxml2) if a '<' is
     * found in the first 512 bytes; falls back to plain-text line scan.
     * converts the resulting byte offset to a character offset so that
     * Python slicing works correctly with multi-byte UTF-8 characters.
     *
     * return: Python int — character index of signature start, or -1
     */
    const char *text;
    Py_ssize_t text_len;
    Py_ssize_t byte_off;
    Py_ssize_t i;
    PyObject *prefix;
    Py_ssize_t char_off;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    byte_off = -1;
    for (i = 0; i < text_len && i < 512; i++) {
        if (text[i] == '<') {
            byte_off = find_sig_in_html(text, (size_t)text_len);
            break;
        }
    }
    if (byte_off < 0)
        byte_off = find_sig_in_plain(text, (size_t)text_len);
    if (byte_off < 0)
        return PyLong_FromLong(-1L);
    prefix = PyUnicode_DecodeUTF8(text, byte_off, "replace");
    if (!prefix)
        return NULL;
    char_off = PyUnicode_GetLength(prefix);
    Py_DECREF(prefix);
    return PyLong_FromSsize_t(char_off);
}

PyObject *py_strip_signature(PyObject *module, PyObject *args) {
    /*
     * module: unused Python module argument
     * args: Python tuple containing one string segment
     *
     * description:
     * calls py_find_signature to locate the signature start index, then
     * returns a slice of the input up to (but not including) that index.
     * if no signature is found, the full input string is returned unchanged.
     *
     * return: Python string with signature removed, or original if none found
     */
    const char *text;
    Py_ssize_t text_len;
    PyObject *idx_obj;
    Py_ssize_t idx;
    PyObject *full;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    idx_obj = py_find_signature(module, args);
    if (!idx_obj)
        return NULL;
    idx = PyLong_AsSsize_t(idx_obj);
    Py_DECREF(idx_obj);
    full = PyUnicode_DecodeUTF8(text, text_len, "replace");
    if (!full)
        return NULL;
    if (idx < 0)
        return full;
    result = PySequence_GetSlice(full, 0, idx);
    Py_DECREF(full);
    return result;
}
