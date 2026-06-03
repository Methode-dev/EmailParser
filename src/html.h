#ifndef EMAILPARSER_HTML_H
#define EMAILPARSER_HTML_H
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "buf.h"
#include <ctype.h>
#include <libxml/tree.h>

int walk_text(xmlNodePtr node, strbuf_t *sb);
PyObject *segment_to_text(const char *html);
char *html_to_plain_c(const char *html);

/* Detect HTML: <tag>, </tag> — not bare < in email addresses */
static inline int looks_like_html(const char *text, size_t len) {
    for (size_t i = 0; i + 1 < len && i < 512; i++) {
        if (text[i] != '<')
            continue;
        if (text[i + 1] == '/')
            return 1;
        if (isalpha((unsigned char)text[i + 1])) {
            size_t j = i + 2;
            while (j < len && isalpha((unsigned char)text[j]))
                j++;
            if (j < len && (text[j] == '>' || text[j] == ' ' || text[j] == '/'))
                return 1;
        }
    }
    return 0;
}
#endif
