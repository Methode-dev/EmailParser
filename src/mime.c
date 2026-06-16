#include "mime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char *decode_qp(const char *in, size_t in_len, size_t *out_len) {
    /*
     * in: quoted-printable encoded input buffer
     * in_len: byte length of in
     * out_len: set to the number of decoded bytes written
     *
     * description:
     * decodes a quoted-printable string — =XX hex sequences become
     * their byte values and soft line breaks (=\n, =\r\n) are removed.
     * the output is always <= in_len bytes.
     *
     * return: malloc'd NUL-terminated decoded string, caller frees
     */
    char *out;
    size_t i;
    size_t j;

    out = malloc(in_len + 1);
    if (!out)
        return NULL;
    i = 0;
    j = 0;
    while (i < in_len) {
        if (in[i] == '=' && i + 1 < in_len) {
            if (in[i + 1] == '\r' && i + 2 < in_len && in[i + 2] == '\n') {
                i += 3;
                continue;
            }
            if (in[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (i + 2 < in_len && isxdigit((unsigned char)in[i + 1]) &&
                isxdigit((unsigned char)in[i + 2])) {
                unsigned int byte;
                sscanf(in + i + 1, "%2x", &byte);
                out[j++] = (char)(unsigned char)byte;
                i += 3;
                continue;
            }
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
    if (out_len)
        *out_len = j;
    return out;
}

char *skip_mime_headers(char *raw) {
    /*
     * raw: start of the raw email buffer
     *
     * description:
     * if the buffer begins with email headers (not HTML), advances past
     * the first blank line so chain-separator search starts from the
     * actual body. scans until the first blank line with no byte limit —
     * modern emails with ARC/DKIM chains can have headers beyond 16 KB.
     *
     * return: pointer to body start, equal to raw if nothing was skipped
     */
    char *p;

    if (*raw == '<')
        return raw;
    p = raw;
    while (*p) {
        if (p[0] == '\n' && p[1] == '\n')
            return p + 2;
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        p++;
    }
    return raw;
}

int has_html_mime_part(const char *text, int len) {
    /*
     * text: content between two From: separators
     * len: byte length of text
     *
     * description:
     * returns 1 if a MIME boundary immediately followed by
     * "Content-type: text/html" is found inside the range, which marks
     * the start of the HTML duplicate of an already-processed plain-text
     * part in a multipart/alternative email.
     *
     * return: 1 if an HTML MIME part opener is found, 0 otherwise
     */
    const char *p;
    const char *end;
    const char *v;

    p = text;
    end = text + len;
    while (p < end) {
        if (p[0] == '-' && p[1] == '-' && p + 2 < end && p[2] != '-' &&
            !isspace((unsigned char)p[2])) {
            while (p < end && *p != '\n')
                p++;
            if (p < end)
                p++;
            while (p < end && (*p == '\r' || *p == '\n'))
                p++;
            if (end - p >= 24 && strncasecmp(p, "Content-type:", 13) == 0) {
                v = p + 13;
                while (v < end && (*v == ' ' || *v == '\t'))
                    v++;
                if (strncasecmp(v, "text/html", 9) == 0)
                    return 1;
            }
        }
        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;
    }
    return 0;
}
