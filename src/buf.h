#ifndef EMAILPARSER_BUF_H
#define EMAILPARSER_BUF_H
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
} strbuf_t;

static inline int sb_push(strbuf_t *sb, const char *s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = (sb->len + n + 1) * 2;
        char *tmp = realloc(sb->buf, cap);
        if (!tmp)
            return -1;
        sb->buf = tmp;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}
#endif
