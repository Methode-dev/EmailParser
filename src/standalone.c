#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "standalone.h"
#include "buf.h"
#include "mime.h"
#include "html.h"
#include <string.h>
#include <stdlib.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

static void collect_style_nodes(xmlNodePtr node, strbuf_t *sb) {
    /*
     * node: root of the XML/HTML subtree to traverse
     * sb: growing string buffer to append style content into
     *
     * description:
     * recursively walks the DOM looking for <style> elements and
     * appends their text content to sb, separated by newlines.
     *
     * return: nothing
     */
    xmlNodePtr cur;
    xmlChar *content;

    for (cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE && cur->name &&
            strcasecmp((const char *)cur->name, "style") == 0) {
            content = xmlNodeGetContent(cur);
            if (content) {
                sb_push(sb, (const char *)content,
                        strlen((const char *)content));
                sb_push(sb, "\n", 1);
                xmlFree(content);
            }
        }
        if (cur->children)
            collect_style_nodes(cur->children, sb);
    }
}

char *extract_css(const char *html, size_t html_len) {
    /*
     * html: raw HTML source to scan
     * html_len: byte length of html
     *
     * description:
     * parses the HTML document with libxml2, then collects the text
     * content of every <style> element via collect_style_nodes.
     *
     * return: malloc'd CSS string, or NULL if no <style> found; caller frees
     */
    htmlDocPtr doc;
    strbuf_t sb;
    xmlNodePtr root;

    doc = htmlReadMemory(html, (int)html_len, NULL, "UTF-8",
                         HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc)
        return NULL;
    sb.buf = NULL;
    sb.len = 0;
    sb.cap = 0;
    root = xmlDocGetRootElement(doc);
    if (root)
        collect_style_nodes(root, &sb);
    xmlFreeDoc(doc);
    return sb.buf;
}

PyObject *wrap_standalone(const char *css, size_t css_len, const char *segment,
                          size_t seg_len) {
    /*
     * css: extracted <style> content to embed (may be empty string)
     * css_len: byte length of css
     * segment: email segment content (HTML or plain text)
     * seg_len: byte length of segment
     *
     * description:
     * wraps segment in a complete HTML document with a base stylesheet
     * and the provided css block. plain-text segments are
     * quoted-printable decoded and wrapped in <pre> for correct
     * rendering; HTML segments are embedded as-is.
     *
     * return: new Python unicode string containing the standalone document
     */
    static const char HEAD[] =
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"UTF-8\">"
        "<style>"
        "body{font-family:sans-serif;margin:1.5em;line-height:1.4}"
        "pre{white-space:pre-wrap;word-break:break-word;"
        "font-family:inherit}";
    static const char MID[] = "</style></head><body>";
    static const char POST[] = "</body></html>";
    int is_html;
    char *decoded;
    size_t decoded_len;
    const char *open;
    const char *close;
    size_t open_len;
    size_t close_len;
    size_t total;
    char *buf;
    char *p;
    PyObject *result;

    is_html = looks_like_html(segment, seg_len);
    decoded = NULL;
    decoded_len = seg_len;
    if (!is_html) {
        decoded = decode_qp(segment, seg_len, &decoded_len);
        if (!decoded) {
            PyErr_NoMemory();
            return NULL;
        }
        segment = decoded;
        seg_len = decoded_len;
    }
    open = is_html ? "" : "<pre>";
    close = is_html ? "" : "</pre>";
    open_len = strlen(open);
    close_len = strlen(close);
    total = (sizeof HEAD - 1) + css_len + (sizeof MID - 1) + open_len +
            seg_len + close_len + sizeof POST;
    buf = malloc(total);
    if (!buf) {
        free(decoded);
        PyErr_NoMemory();
        return NULL;
    }
    p = buf;
    memcpy(p, HEAD, sizeof HEAD - 1);
    p += sizeof HEAD - 1;
    memcpy(p, css, css_len);
    p += css_len;
    memcpy(p, MID, sizeof MID - 1);
    p += sizeof MID - 1;
    memcpy(p, open, open_len);
    p += open_len;
    memcpy(p, segment, seg_len);
    p += seg_len;
    memcpy(p, close, close_len);
    p += close_len;
    memcpy(p, POST, sizeof POST - 1);
    p += sizeof POST - 1;
    *p = '\0';
    free(decoded);
    result = PyUnicode_FromStringAndSize(buf, (Py_ssize_t)(p - buf));
    free(buf);
    return result;
}
