#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "headers.h"
#include "buf.h"
#include "mime.h"
#include "html.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

const char *canonical_key(const char *name, size_t len) {
    /*
     * name: header field name as read from the email segment
     * len: byte length of name
     *
     * description:
     * lowercases ASCII bytes of name (non-ASCII bytes kept as-is for
     * UTF-8 French variants such as À and Envoyé), then looks up the
     * result in the EN/FR field mapping table.
     *
     * return: canonical key string ("from", "to", "cc", "bcc",
     *         "subject", "date"), or NULL if unrecognised
     */
    static const struct {
        const char *raw;
        const char *key;
    } MAP[] = {
        {"from", "from"},     {"reply-to", "from"}, {"to", "to"},
        {"cc", "cc"},         {"bcc", "bcc"},       {"subject", "subject"},
        {"date", "date"},     {"sent", "date"},     {"de", "from"},
        {"a", "to"},          {"\xc3\x80", "to"},   {"\xc3\xa0", "to"},
        {"objet", "subject"}, {"cci", "bcc"},       {"envoy\xc3\xa9", "date"},
        {NULL, NULL}};
    char lower[64];
    size_t n;
    int i;

    lower[0] = '\0';
    n = len < 63 ? len : 63;
    for (size_t j = 0; j < n; j++)
        lower[j] = (unsigned char)name[j] < 0x80
                       ? (char)tolower((unsigned char)name[j])
                       : name[j];
    lower[n] = '\0';
    for (i = 0; MAP[i].raw; i++)
        if (strcmp(lower, MAP[i].raw) == 0)
            return MAP[i].key;
    return NULL;
}

static void split_recipients(PyObject *list, const char *v, size_t vlen) {
    /*
     * list: Python list to append each recipient string to
     * v: raw recipient field value
     * vlen: byte length of v
     *
     * description:
     * scans v for comma or semicolon delimiters, respecting angle-bracket
     * and double-quote nesting. each trimmed non-empty token is decoded
     * as UTF-8 and appended to list.
     *
     * return: nothing
     */
    int in_angle;
    int in_quote;
    const char *start;
    const char *end;
    const char *s;
    const char *p;
    size_t n;
    char c;
    PyObject *item;

    in_angle = 0;
    in_quote = 0;
    start = v;
    end = v + vlen;
    for (p = v; p <= end; p++) {
        c = (p < end) ? *p : ',';
        if (!in_quote && c == '<') {
            in_angle++;
            continue;
        }
        if (!in_quote && c == '>') {
            if (in_angle > 0)
                in_angle--;
            continue;
        }
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && !in_angle && (c == ',' || c == ';' || p == end)) {
            s = start;
            n = (size_t)(p - start);
            while (n > 0 && isspace((unsigned char)*s)) {
                s++;
                n--;
            }
            while (n > 0 && isspace((unsigned char)s[n - 1]))
                n--;
            if (n > 0) {
                item = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "replace");
                if (item) {
                    PyList_Append(list, item);
                    Py_DECREF(item);
                }
            }
            start = p + 1;
        }
    }
}

static void flush_field(PyObject *dict, const char *key, strbuf_t *sb) {
    /*
     * dict: result dictionary to write into
     * key: canonical field key, or NULL to no-op
     * sb: accumulated raw field value buffer
     *
     * description:
     * trims whitespace from the buffered value. for list fields (to, cc,
     * bcc) it splits and appends recipients; for string fields it sets
     * the value only if not already set (first occurrence wins).
     * always resets sb to empty and frees its buffer.
     *
     * return: nothing
     */
    const char *v;
    size_t n;
    PyObject *lst;
    PyObject *existing;
    PyObject *s;

    if (!key)
        goto cleanup;
    v = sb->buf ? sb->buf : "";
    n = sb->len;
    while (n > 0 && isspace((unsigned char)*v)) {
        v++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)v[n - 1]))
        n--;
    if (n == 0)
        goto cleanup;
    if (strcmp(key, "to") == 0 || strcmp(key, "cc") == 0 ||
        strcmp(key, "bcc") == 0) {
        lst = PyDict_GetItemString(dict, key);
        if (lst)
            split_recipients(lst, v, n);
    } else {
        existing = PyDict_GetItemString(dict, key);
        if (existing == Py_None) {
            s = PyUnicode_DecodeUTF8(v, (Py_ssize_t)n, "replace");
            if (s) {
                PyDict_SetItemString(dict, key, s);
                Py_DECREF(s);
            }
        }
    }
cleanup:
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

PyObject *py_parse_headers(PyObject *module, PyObject *args) {
    /*
     * module: unused Python module argument
     * args: Python tuple containing one string segment
     *
     * description:
     * extracts From/To/CC/BCC/Subject/Date fields from an email segment.
     * HTML segments are first converted to plain text via libxml2;
     * all content is quoted-printable decoded before parsing.
     * lines are scanned until the first blank line (header/body separator).
     *
     * return: Python dict with keys "from", "to", "cc", "bcc",
     *         "subject", "date" (string fields default to None,
     *         list fields default to [])
     */
    static const char *LIST_KEYS[] = {"to", "cc", "bcc", NULL};
    static const char *STR_KEYS[] = {"from", "subject", "date", NULL};
    const char *text;
    Py_ssize_t text_len;
    int is_html;
    char *plain;
    size_t plain_len;
    size_t decoded_len;
    char *decoded;
    PyObject *result;
    PyObject *lst;
    const char *p;
    const char *end;
    const char *cur_key;
    strbuf_t cur_val;
    const char *ls;
    size_t llen;
    const char *colon;
    const char *fname;
    size_t flen;
    const char *val;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    is_html = looks_like_html(text, (size_t)text_len);
    if (is_html) {
        plain = html_to_plain_c(text);
        if (!plain) {
            PyErr_NoMemory();
            return NULL;
        }
        plain_len = strlen(plain);
    } else {
        plain = malloc((size_t)text_len + 1);
        if (!plain) {
            PyErr_NoMemory();
            return NULL;
        }
        memcpy(plain, text, (size_t)text_len);
        plain[(size_t)text_len] = '\0';
        plain_len = (size_t)text_len;
    }
    decoded = decode_qp(plain, plain_len, &decoded_len);
    free(plain);
    if (!decoded) {
        PyErr_NoMemory();
        return NULL;
    }
    result = PyDict_New();
    if (!result) {
        free(decoded);
        return NULL;
    }
    for (int i = 0; LIST_KEYS[i]; i++) {
        lst = PyList_New(0);
        if (!lst || PyDict_SetItemString(result, LIST_KEYS[i], lst) < 0) {
            Py_XDECREF(lst);
            Py_DECREF(result);
            free(decoded);
            return NULL;
        }
        Py_DECREF(lst);
    }
    for (int i = 0; STR_KEYS[i]; i++)
        if (PyDict_SetItemString(result, STR_KEYS[i], Py_None) < 0) {
            Py_DECREF(result);
            free(decoded);
            return NULL;
        }
    p = decoded;
    end = decoded + decoded_len;
    while (p < end && (*p == '\n' || *p == '\r'))
        p++;
    cur_key = NULL;
    cur_val.buf = NULL;
    cur_val.len = 0;
    cur_val.cap = 0;
    while (p <= end) {
        ls = p;
        while (p < end && *p != '\n')
            p++;
        llen = (size_t)(p - ls);
        if (llen > 0 && ls[llen - 1] == '\r')
            llen--;
        if (p < end)
            p++;
        if (llen == 0) {
            flush_field(result, cur_key, &cur_val);
            cur_key = NULL;
            break;
        }
        if (cur_key && isspace((unsigned char)ls[0])) {
            const char *s = ls;
            size_t n = llen;
            while (n > 0 && isspace((unsigned char)*s)) {
                s++;
                n--;
            }
            sb_push(&cur_val, " ", 1);
            sb_push(&cur_val, s, n);
            continue;
        }
        flush_field(result, cur_key, &cur_val);
        cur_key = NULL;
        colon = memchr(ls, ':', llen);
        if (!colon)
            continue;
        fname = ls;
        flen = (size_t)(colon - ls);
        while (flen > 0 && isspace((unsigned char)fname[flen - 1]))
            flen--;
        cur_key = canonical_key(fname, flen);
        if (!cur_key)
            continue;
        val = colon + 1;
        sb_push(&cur_val, val, llen - (size_t)(val - ls));
    }
    flush_field(result, cur_key, &cur_val);
    free(decoded);
    return result;
}
