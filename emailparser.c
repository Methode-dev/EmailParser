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

/* If raw begins with MIME/email headers (not HTML), advance past the first
   blank line so chain-separator search starts from the actual body.
   Returns the body start pointer (may equal raw if no skip was done). */
static char *skip_mime_headers(char *raw)
{
    if (*raw == '<') return raw;   /* HTML — no outer headers to skip */
    char *p = raw;
    while (*p) {
        if (p[0] == '\n' && p[1] == '\n')
            return p + 2;
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        if ((size_t)(p - raw) > 8192) break;   /* no blank line in 8 KB — give up */
        p++;
    }
    return raw;
}

static email_t *new_email(char *raw)
{
    email_t *e = malloc(sizeof(email_t));
    if (!e)
        return NULL;
    e->body                = raw;
    e->last_index          = 0;
    e->exhausted           = 0;
    e->yield_if_empty_chain = 0;
    return e;
}

/* Return 1 if [text, text+len) contains a MIME boundary immediately followed
   by "Content-type: text/html", which marks the start of the HTML duplicate
   of an already-processed plain-text part in a multipart/alternative email. */
static int has_html_mime_part(const char *text, int len)
{
    const char *p   = text;
    const char *end = text + len;
    while (p < end) {
        if (p[0] == '-' && p[1] == '-' && p + 2 < end &&
            p[2] != '-' && !isspace((unsigned char)p[2])) {
            /* skip to start of next line */
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            while (p < end && (*p == '\r' || *p == '\n')) p++;
            /* check for Content-type: text/html */
            if (end - p >= 24 && strncasecmp(p, "Content-type:", 13) == 0) {
                const char *v = p + 13;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                if (strncasecmp(v, "text/html", 9) == 0)
                    return 1;
            }
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return 0;
}

static bool get_next_val(email_t *email)
{
    if (email->exhausted) return false;
    int had_sep = (email->last_index != 0);
    email->body += email->last_index;
    if (!*email->body) return false;   /* nothing left */
    int idx = get_index_sep(email->body + 1);
    if (idx < 0) {
        /* No further separator.  Yield tail if: we already crossed a separator
           (chain tail), OR the file had its headers skipped (single email). */
        if (!had_sep && !email->yield_if_empty_chain) return false;
        email->exhausted = 1;
        return true;
    }
    if (has_html_mime_part(email->body + 1, idx)) {
        /* yield this last segment, stop on next call */
        if (idx >= 1) email->body[idx - 1] = '\0';
        email->exhausted = 1;
        return true;
    }
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

/* C-level HTML→plain helper (malloc'd string, caller frees). */
static char *html_to_plain_c(const char *html)
{
    htmlDocPtr doc = htmlReadMemory(html, (int)strlen(html), NULL, "UTF-8",
                                   HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return strdup(html);
    strbuf_t   sb   = {NULL, 0, 0};
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) walk_text(root, &sb);
    xmlFreeDoc(doc);
    return sb.buf ? sb.buf : strdup("");
}

/* ── Standalone HTML wrapping ────────────────────────────────────────────── */

static void collect_style_nodes(xmlNodePtr node, strbuf_t *sb)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE && cur->name &&
            strcasecmp((const char *)cur->name, "style") == 0) {
            xmlChar *content = xmlNodeGetContent(cur);
            if (content) {
                sb_push(sb, (const char *)content, strlen((const char *)content));
                sb_push(sb, "\n", 1);
                xmlFree(content);
            }
        }
        if (cur->children)
            collect_style_nodes(cur->children, sb);
    }
}

/* Parse html, collect all <style> text nodes, return malloc'd CSS or NULL. */
static char *extract_css(const char *html, size_t html_len)
{
    htmlDocPtr doc = htmlReadMemory(html, (int)html_len, NULL, "UTF-8",
                                   HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return NULL;

    strbuf_t   sb   = {NULL, 0, 0};
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) collect_style_nodes(root, &sb);
    xmlFreeDoc(doc);
    return sb.buf;   /* NULL if no <style> found; caller frees */
}

/* ── Quoted-printable decoder ────────────────────────────────────────────── */

/* Decode a quoted-printable string. Output is always <= input length.
   Returns a malloc'd, NUL-terminated buffer; sets *out_len. Caller frees. */
static char *decode_qp(const char *in, size_t in_len, size_t *out_len)
{
    char  *out = malloc(in_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < in_len) {
        if (in[i] == '=' && i + 1 < in_len) {
            /* soft line break:  =\r\n  or  =\n */
            if (in[i + 1] == '\r' && i + 2 < in_len && in[i + 2] == '\n') {
                i += 3; continue;
            }
            if (in[i + 1] == '\n') {
                i += 2; continue;
            }
            /* encoded byte: =XX */
            if (i + 2 < in_len &&
                isxdigit((unsigned char)in[i + 1]) &&
                isxdigit((unsigned char)in[i + 2])) {
                unsigned int byte;
                sscanf(in + i + 1, "%2x", &byte);
                out[j++] = (char)(unsigned char)byte;
                i += 3; continue;
            }
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* Wrap a segment in a complete HTML document.
   Plain-text content (no '<' in the first 512 bytes) is put in <pre> so
   newlines are preserved. A minimal base CSS is always included before the
   extracted stylesheet. */
static PyObject *wrap_standalone(const char *css, size_t css_len,
                                  const char *segment, size_t seg_len)
{
    static const char HEAD[] =
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"UTF-8\">"
        "<style>"
        "body{font-family:sans-serif;margin:1.5em;line-height:1.4}"
        "pre{white-space:pre-wrap;word-break:break-word;font-family:inherit}";
    static const char MID[]  = "</style></head><body>";
    static const char POST[] = "</body></html>";

    /* Detect HTML: look for </tag> or <tagname> / <tagname attr>, not bare
       '<' in email addresses like <user@host>.  A real HTML tag has only
       alpha chars between '<' and the first space / '>' / '/'. */
    int is_html = 0;
    for (size_t i = 0; i + 1 < seg_len && i < 512 && !is_html; i++) {
        if (segment[i] != '<') continue;
        if (segment[i + 1] == '/') { is_html = 1; break; }   /* </tag> */
        if (isalpha((unsigned char)segment[i + 1])) {
            size_t j = i + 2;
            while (j < seg_len && isalpha((unsigned char)segment[j])) j++;
            if (j < seg_len && (segment[j] == '>' || segment[j] == ' '
                                || segment[j] == '/'))
                is_html = 1;
        }
    }

    /* For plain-text segments, decode quoted-printable before displaying. */
    char  *decoded     = NULL;
    size_t decoded_len = seg_len;
    if (!is_html) {
        decoded = decode_qp(segment, seg_len, &decoded_len);
        if (!decoded) { PyErr_NoMemory(); return NULL; }
        segment = decoded;
        seg_len = decoded_len;
    }

    const char *open  = is_html ? "" : "<pre>";
    const char *close = is_html ? "" : "</pre>";
    size_t open_len   = strlen(open);
    size_t close_len  = strlen(close);

    size_t total = (sizeof HEAD - 1) + css_len
                 + (sizeof MID  - 1) + open_len
                 + seg_len + close_len + sizeof POST;
    char *buf = malloc(total);
    if (!buf) { free(decoded); PyErr_NoMemory(); return NULL; }

    char *p = buf;
    memcpy(p, HEAD,  sizeof HEAD - 1); p += sizeof HEAD - 1;
    memcpy(p, css,   css_len);          p += css_len;
    memcpy(p, MID,   sizeof MID  - 1); p += sizeof MID  - 1;
    memcpy(p, open,  open_len);         p += open_len;
    memcpy(p, segment, seg_len);        p += seg_len;
    memcpy(p, close, close_len);        p += close_len;
    memcpy(p, POST,  sizeof POST - 1); p += sizeof POST - 1;
    *p = '\0';

    free(decoded);
    PyObject *result = PyUnicode_FromStringAndSize(buf, (Py_ssize_t)(p - buf));
    free(buf);
    return result;
}

/* ── Header extraction ───────────────────────────────────────────────────── */

/* Map a field name (ASCII-lowercased, UTF-8 non-ASCII kept as-is) to a
   canonical key, or NULL if unrecognised. */
static const char *canonical_key(const char *name, size_t len)
{
    char lower[64] = {0};
    size_t n = len < 63 ? len : 63;
    for (size_t i = 0; i < n; i++)
        lower[i] = (unsigned char)name[i] < 0x80
                   ? (char)tolower((unsigned char)name[i])
                   : name[i];
    lower[n] = '\0';

    static const struct { const char *raw; const char *key; } MAP[] = {
        /* English */
        {"from",           "from"}, {"reply-to",        "from"},
        {"to",             "to"},
        {"cc",             "cc"},   {"bcc",              "bcc"},
        {"subject",        "subject"},
        {"date",           "date"}, {"sent",             "date"},
        /* French */
        {"de",             "from"},
        {"a",              "to"},
        {"\xc3\x80",       "to"},   /* À  (U+00C0) */
        {"\xc3\xa0",       "to"},   /* à  (U+00E0) */
        {"objet",          "subject"},
        {"cci",            "bcc"},
        {"envoy\xc3\xa9",  "date"}, /* Envoyé */
        {NULL, NULL}
    };
    for (int i = 0; MAP[i].raw; i++)
        if (strcmp(lower, MAP[i].raw) == 0)
            return MAP[i].key;
    return NULL;
}

/* Split a recipient value on commas/semicolons (respects angle-bracket and
   quoted-string delimiters) and append each item to list. */
static void split_recipients(PyObject *list, const char *v, size_t vlen)
{
    int in_angle = 0, in_quote = 0;
    const char *start = v, *end = v + vlen;

    for (const char *p = v; p <= end; p++) {
        char c = (p < end) ? *p : ',';
        if (!in_quote && c == '<') { in_angle++; continue; }
        if (!in_quote && c == '>') { if (in_angle > 0) in_angle--; continue; }
        if (c == '"')              { in_quote = !in_quote; continue; }
        if (!in_quote && !in_angle && (c == ',' || c == ';' || p == end)) {
            const char *s = start;
            size_t      n = (size_t)(p - start);
            while (n > 0 && isspace((unsigned char)*s))    { s++; n--; }
            while (n > 0 && isspace((unsigned char)s[n-1])) n--;
            if (n > 0) {
                PyObject *item = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "replace");
                if (item) { PyList_Append(list, item); Py_DECREF(item); }
            }
            start = p + 1;
        }
    }
}

/* Flush the accumulated value for cur_key into dict, then reset the buffer. */
static void flush_field(PyObject *dict, const char *key, strbuf_t *sb)
{
    if (!key) goto cleanup;

    const char *v = sb->buf ? sb->buf : "";
    size_t      n = sb->len;
    while (n > 0 && isspace((unsigned char)*v))    { v++; n--; }
    while (n > 0 && isspace((unsigned char)v[n-1])) n--;
    if (n == 0) goto cleanup;

    if (strcmp(key, "to") == 0 || strcmp(key, "cc") == 0 || strcmp(key, "bcc") == 0) {
        PyObject *lst = PyDict_GetItemString(dict, key);
        if (lst) split_recipients(lst, v, n);
    } else {
        PyObject *existing = PyDict_GetItemString(dict, key);
        if (existing == Py_None) {      /* keep first occurrence only */
            PyObject *s = PyUnicode_DecodeUTF8(v, (Py_ssize_t)n, "replace");
            if (s) { PyDict_SetItemString(dict, key, s); Py_DECREF(s); }
        }
    }
cleanup:
    free(sb->buf);
    sb->buf = NULL; sb->len = 0; sb->cap = 0;
}

static PyObject *py_parse_headers(PyObject *module, PyObject *args)
{
    const char *text;
    Py_ssize_t  text_len;
    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;

    /* ── 1. Convert HTML to plain text if needed ── */
    int is_html = 0;
    for (Py_ssize_t i = 0; i + 1 < text_len && i < 512 && !is_html; i++) {
        if (text[i] != '<') continue;
        if (text[i+1] == '/') { is_html = 1; break; }
        if (isalpha((unsigned char)text[i+1])) {
            Py_ssize_t j = i + 2;
            while (j < text_len && isalpha((unsigned char)text[j])) j++;
            if (j < text_len &&
                (text[j] == '>' || text[j] == ' ' || text[j] == '/'))
                is_html = 1;
        }
    }

    char  *plain     = NULL;
    size_t plain_len = 0;
    if (is_html) {
        plain = html_to_plain_c(text);
        if (!plain) { PyErr_NoMemory(); return NULL; }
        plain_len = strlen(plain);
    } else {
        plain = malloc((size_t)text_len + 1);
        if (!plain) { PyErr_NoMemory(); return NULL; }
        memcpy(plain, text, (size_t)text_len);
        plain[(size_t)text_len] = '\0';
        plain_len = (size_t)text_len;
    }

    /* ── 2. Decode quoted-printable ── */
    size_t decoded_len;
    char  *decoded = decode_qp(plain, plain_len, &decoded_len);
    free(plain);
    if (!decoded) { PyErr_NoMemory(); return NULL; }

    /* ── 3. Build result dict with default values ── */
    PyObject *result = PyDict_New();
    if (!result) { free(decoded); return NULL; }

    static const char *LIST_KEYS[] = {"to", "cc", "bcc", NULL};
    static const char *STR_KEYS[]  = {"from", "subject", "date", NULL};
    for (int i = 0; LIST_KEYS[i]; i++) {
        PyObject *lst = PyList_New(0);
        if (!lst || PyDict_SetItemString(result, LIST_KEYS[i], lst) < 0) {
            Py_XDECREF(lst); goto fail;
        }
        Py_DECREF(lst);
    }
    for (int i = 0; STR_KEYS[i]; i++) {
        if (PyDict_SetItemString(result, STR_KEYS[i], Py_None) < 0) goto fail;
    }

    /* ── 4. Scan lines until blank line (header/body separator) ── */
    {
        const char *p = decoded, *end = decoded + decoded_len;
        /* skip any leading blank lines (segments often start with \n) */
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        const char *cur_key = NULL;
        strbuf_t    cur_val = {NULL, 0, 0};

        while (p <= end) {
            const char *ls = p;
            while (p < end && *p != '\n') p++;
            size_t llen = (size_t)(p - ls);
            if (llen > 0 && ls[llen-1] == '\r') llen--;
            if (p < end) p++;

            if (llen == 0) {            /* blank line → header block ends */
                flush_field(result, cur_key, &cur_val);
                cur_key = NULL;
                break;
            }
            if (cur_key && isspace((unsigned char)ls[0])) {  /* continuation */
                const char *s = ls; size_t n = llen;
                while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
                sb_push(&cur_val, " ", 1);
                sb_push(&cur_val, s, n);
                continue;
            }

            flush_field(result, cur_key, &cur_val);
            cur_key = NULL;

            const char *colon = memchr(ls, ':', llen);
            if (!colon) continue;

            const char *fname = ls;
            size_t      flen  = (size_t)(colon - ls);
            while (flen > 0 && isspace((unsigned char)fname[flen-1])) flen--;

            cur_key = canonical_key(fname, flen);
            if (!cur_key) continue;

            const char *val  = colon + 1;
            size_t      vlen = llen - (size_t)(val - ls);
            sb_push(&cur_val, val, vlen);
        }
        flush_field(result, cur_key, &cur_val);  /* flush last field if no blank line */
    }

    free(decoded);
    return result;

fail:
    Py_DECREF(result);
    free(decoded);
    return NULL;
}

/* ── Header stripping ────────────────────────────────────────────────────── */

/* If text starts (after leading blank lines) with a recognised email header
   field, scan to the first blank line and return a pointer to the body that
   follows it.  Returns text unchanged if no header block is detected. */
static const char *find_body_start(const char *text, size_t len)
{
    const char *p = text, *end = text + len;

    /* skip leading blank lines */
    while (p < end && (*p == '\n' || *p == '\r')) p++;

    /* check that the first non-blank line starts with a recognised field name */
    {
        const char *q = p;
        while (q < end && *q != '\n' && *q != ':') q++;
        if (q >= end || *q != ':') return text;

        /* trim trailing whitespace from field name (handles "De :", "À :") */
        const char *fname = p;
        size_t      flen  = (size_t)(q - p);
        while (flen > 0 && isspace((unsigned char)fname[flen-1])) flen--;
        if (flen == 0) return text;

        /* field name must not contain internal spaces */
        for (size_t i = 0; i < flen; i++)
            if (isspace((unsigned char)fname[i])) return text;

        if (!canonical_key(fname, flen)) return text;   /* unrecognised → no strip */
    }

    /* scan forward to the blank line that ends the header block */
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        size_t llen = (size_t)(p - ls);
        if (llen > 0 && ls[llen-1] == '\r') llen--;
        if (p < end) p++;
        if (llen == 0) return p;   /* blank line found — body starts here */
    }
    return text;   /* no blank line after headers, return unchanged */
}

static PyObject *py_find_signature(PyObject *module, PyObject *args);  /* forward */

static PyObject *py_strip_signature(PyObject *module, PyObject *args)
{
    const char *text;
    Py_ssize_t  text_len;
    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;

    /* reuse find_signature to get the char offset */
    PyObject *idx_obj = py_find_signature(module, args);
    if (!idx_obj) return NULL;
    Py_ssize_t idx = PyLong_AsSsize_t(idx_obj);
    Py_DECREF(idx_obj);

    PyObject *full = PyUnicode_DecodeUTF8(text, text_len, "replace");
    if (!full) return NULL;
    if (idx < 0) return full;   /* no signature — return unchanged */

    PyObject *result = PySequence_GetSlice(full, 0, idx);
    Py_DECREF(full);
    return result;
}

static PyObject *py_extract_body(PyObject *module, PyObject *args)
{
    const char *text;
    Py_ssize_t  text_len;
    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    const char *body = find_body_start(text, (size_t)text_len);
    return PyUnicode_DecodeUTF8(body,
                                (Py_ssize_t)(text_len - (body - text)),
                                "replace");
}

/* ── Python type ─────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    email_t *email;
    char    *raw;
    char    *css;              /* extracted <style> content, NULL if not standalone */
    size_t   css_len;
    size_t   outer_hdr_len;   /* bytes of outer MIME headers skipped at init (0 = none) */
    int      exhausted;
    int      plain_text;
    int      standalone;
    int      strip_headers;
} EmailObject;

static void Email_dealloc(EmailObject *self)
{
    free(self->email);
    free(self->raw);
    free(self->css);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Email_init(EmailObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"source", "plain_text", "standalone", "strip_headers", NULL};

    /* support re-init */
    free(self->email); self->email = NULL;
    free(self->raw);   self->raw   = NULL;
    free(self->css);   self->css   = NULL;
    self->css_len       = 0;
    self->outer_hdr_len = 0;
    self->exhausted     = 0;
    self->plain_text    = 0;
    self->standalone    = 0;
    self->strip_headers = 0;

    PyObject *input;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ppp", kwlist,
                                     &input, &self->plain_text, &self->standalone,
                                     &self->strip_headers))
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

    char *body = skip_mime_headers(raw);
    if (body != raw) {
        self->email->body                = body;
        self->email->yield_if_empty_chain = 1;
        self->outer_hdr_len              = (size_t)(body - raw);
    }

    if (self->standalone && !self->plain_text) {
        self->css     = extract_css(raw, (size_t)size);
        self->css_len = self->css ? strlen(self->css) : 0;
    }
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
    const char *body = self->email->body;
    if (self->strip_headers)
        body = find_body_start(body, strlen(body));

    if (self->plain_text)
        return segment_to_text(body);
    if (self->standalone) {
        const char *css = self->css ? self->css : "";
        return wrap_standalone(css, self->css_len, body, strlen(body));
    }
    return PyUnicode_FromString(body);
}

static PyObject *Email_get_outer_headers(EmailObject *self, void *closure)
{
    if (self->outer_hdr_len == 0)
        Py_RETURN_NONE;
    PyObject *arg = Py_BuildValue("(s#)", self->raw, (Py_ssize_t)self->outer_hdr_len);
    if (!arg) return NULL;
    PyObject *result = py_parse_headers(NULL, arg);
    Py_DECREF(arg);
    return result;
}

static PyGetSetDef Email_getset[] = {
    {"outer_headers", (getter)Email_get_outer_headers, NULL,
     PyDoc_STR(
         "Parsed headers of the most recent email (From, To, CC, Subject, Date)\n"
         "extracted from the outer MIME header block.\n"
         "Returns None for pure HTML emails that have no outer MIME headers."
     ), NULL},
    {NULL}
};

static PyTypeObject EmailType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "emailparser.Email",
    .tp_basicsize = sizeof(EmailObject),
    .tp_dealloc   = (destructor)Email_dealloc,
    .tp_iter      = (getiterfunc)Email_iter,
    .tp_iternext  = (iternextfunc)Email_next,
    .tp_init      = (initproc)Email_init,
    .tp_new       = PyType_GenericNew,
    .tp_getset    = Email_getset,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = PyDoc_STR(
        "Email(source, plain_text=False, standalone=False, strip_headers=False)\n\n"
        "  source:        file path (str), raw content (str), or raw bytes\n"
        "  plain_text:    strip HTML tags and decode entities via libxml2\n"
        "  standalone:    wrap each HTML segment in a full document with\n"
        "                 extracted <style> CSS. Ignored when plain_text=True.\n"
        "  strip_headers: remove the From/To/Subject/Date header block from\n"
        "                 the top of each quoted segment, leaving only the body.\n"
        "                 Has no effect on segments that do not begin with a\n"
        "                 recognised header field (e.g. the first segment).\n\n"
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
    {"strip_signature", py_strip_signature, METH_VARARGS,
     PyDoc_STR(
         "strip_signature(text) -> str\n\n"
         "Return text with the signature block removed.\n"
         "If no signature is detected the input is returned unchanged.\n\n"
         "Pair with find_signature() if you need the signature text itself:\n"
         "  idx = emailparser.find_signature(seg)\n"
         "  sig = seg[idx:] if idx >= 0 else ''"
     )},
    {"extract_body", py_extract_body, METH_VARARGS,
     PyDoc_STR(
         "extract_body(segment) -> str\n\n"
         "Return the body of an email segment, stripping the From/To/Subject/Date\n"
         "header block from the top.  If the segment does not begin with a\n"
         "recognised header field (e.g. the first segment), it is returned as-is.\n\n"
         "Pair with parse_headers() to access both parts:\n"
         "  headers = emailparser.parse_headers(seg)\n"
         "  body    = emailparser.extract_body(seg)"
     )},
    {"parse_headers", py_parse_headers, METH_VARARGS,
     PyDoc_STR(
         "parse_headers(segment) -> dict\n\n"
         "Extract header fields from an email segment.\n\n"
         "Returns a dict with keys:\n"
         "  'from'    : str | None\n"
         "  'to'      : list[str]\n"
         "  'cc'      : list[str]\n"
         "  'bcc'     : list[str]\n"
         "  'subject' : str | None\n"
         "  'date'    : str | None\n\n"
         "Recognises both English (From, To, CC, BCC, Subject, Date, Sent)\n"
         "and French (De, \xc3\x80/\xc3\xa0, Cci, Objet, Envoy\xc3\xa9) field names.\n"
         "Handles HTML segments and quoted-printable encoding automatically."
     )},
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
