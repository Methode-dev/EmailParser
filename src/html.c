#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "html.h"
#include <string.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

static const char *BLOCK_TAGS[] = {"p",  "div", "br", "tr", "li", "h1",
                                   "h2", "h3",  "h4", "h5", "h6", NULL};

static int is_block(const char *name) {
    /*
     * name: XML element name string
     *
     * description:
     * checks whether name matches one of the block-level HTML tags
     * that should emit a newline when extracting plain text.
     *
     * return: 1 if name is a block tag, 0 otherwise
     */
    int i;

    for (i = 0; BLOCK_TAGS[i]; i++)
        if (!strcasecmp(name, BLOCK_TAGS[i]))
            return 1;
    return 0;
}

int walk_text(xmlNodePtr node, strbuf_t *sb) {
    /*
     * node: root of the XML/HTML subtree to traverse
     * sb: growing string buffer to append text content into
     *
     * description:
     * recursively walks every node in the DOM tree. text nodes have
     * their content appended directly; block-level elements emit a
     * newline before descending into their children.
     *
     * return: 0 on success, -1 on allocation failure
     */
    xmlNodePtr cur;
    const char *s;

    for (cur = node; cur; cur = cur->next) {
        if (cur->type == XML_TEXT_NODE && cur->content) {
            s = (const char *)cur->content;
            if (sb_push(sb, s, strlen(s)) < 0)
                return -1;
        } else if (cur->type == XML_ELEMENT_NODE) {
            if (is_block((const char *)cur->name))
                if (sb_push(sb, "\n", 1) < 0)
                    return -1;
            if (cur->children && walk_text(cur->children, sb) < 0)
                return -1;
        }
    }
    return 0;
}

PyObject *segment_to_text(const char *html) {
    /*
     * html: raw HTML string to convert
     *
     * description:
     * parses html with libxml2's lenient HTML parser, walks the DOM
     * with walk_text and returns the concatenated plain-text content.
     * falls back to returning the input unchanged if parsing fails.
     *
     * return: new Python unicode string with plain-text content
     */
    htmlDocPtr doc;
    strbuf_t sb;
    xmlNodePtr root;
    PyObject *result;

    doc = htmlReadMemory(html, (int)strlen(html), NULL, "UTF-8",
                         HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc)
        return PyUnicode_FromString(html);
    sb.buf = NULL;
    sb.len = 0;
    sb.cap = 0;
    root = xmlDocGetRootElement(doc);
    if (root && walk_text(root, &sb) < 0) {
        xmlFreeDoc(doc);
        free(sb.buf);
        PyErr_NoMemory();
        return NULL;
    }
    xmlFreeDoc(doc);
    result = PyUnicode_FromStringAndSize(sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    return result;
}

char *html_to_plain_c(const char *html) {
    /*
     * html: raw HTML string to convert
     *
     * description:
     * C-level counterpart to segment_to_text. parses html with libxml2
     * and returns the extracted plain text as a malloc'd C string.
     * falls back to strdup(html) if parsing fails.
     *
     * return: malloc'd NUL-terminated plain-text string, caller frees
     */
    htmlDocPtr doc;
    strbuf_t sb;
    xmlNodePtr root;

    doc = htmlReadMemory(html, (int)strlen(html), NULL, "UTF-8",
                         HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc)
        return strdup(html);
    sb.buf = NULL;
    sb.len = 0;
    sb.cap = 0;
    root = xmlDocGetRootElement(doc);
    if (root)
        walk_text(root, &sb);
    xmlFreeDoc(doc);
    return sb.buf ? sb.buf : strdup("");
}
