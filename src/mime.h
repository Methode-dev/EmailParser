#ifndef EMAILPARSER_MIME_H
#define EMAILPARSER_MIME_H
#include <stddef.h>

char *decode_qp(const char *in, size_t in_len, size_t *out_len);
char *skip_mime_headers(char *raw);
int has_html_mime_part(const char *text, int len);
#endif
