#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include <stdbool.h>
#include <fcntl.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include "email.h"

#ifndef SEPARATOR_REGEX
    #define SEPARATOR_REGEX_GEN_EN "(From|Sent|To|Subject|Cc|Bcc) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX_GEN_FR "(De|À|Envoyé|Objet|Cc|Cci) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX_STA_ALL "(De|From) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX_END_ALL "(Objet|Subject) ?(&nbsp;:|:) ?"
    #define SEPARATOR_REGEX SEPARATOR_REGEX_STA_ALL
#endif


int get_index_sep(char *email)
{
    regex_t regex;
    regmatch_t match;

    if (regcomp(&regex, SEPARATOR_REGEX, REG_EXTENDED) != 0)
        return -1;
    int ret = regexec(&regex, email, 1, &match, 0);
    regfree(&regex);
    if (ret == REG_NOMATCH)
        return -1;
    return match.rm_so;
}

int find_char(char tok, char *str)
{
    int i = 0;
    for (; str[i] && str[i] != tok; i++);
    return i;
}

static int has_html_mime_part(const char *text, int len)
{
    const char *p   = text;
    const char *end = text + len;
    while (p < end) {
        if (p[0] == '-' && p[1] == '-' && p + 2 < end &&
            p[2] != '-' && !isspace((unsigned char)p[2])) {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            while (p < end && (*p == '\r' || *p == '\n')) p++;
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

bool get_next_val(email_t *email)
{
    if (email->exhausted) return false;
    int had_sep = (email->last_index != 0);
    email->body += email->last_index;
    if (!*email->body) return false;
    int idx = get_index_sep(email->body + 1);
    if (idx < 0) {
        if (!had_sep) return false;
        email->exhausted = 1;
        return true;
    }
    if (has_html_mime_part(email->body + 1, idx)) {
        if (idx >= 1) email->body[idx - 1] = '\0';
        email->exhausted = 1;
        return true;
    }
    email->last_index = idx + 1;  /* +1: idx is relative to body+1 */
    if (email->last_index >= 2)
        email->body[email->last_index - 2] = '\0';
    return true;
}

static char *skip_mime_headers(char *raw)
{
    if (*raw == '<') return raw;
    char *p = raw;
    while (*p) {
        if (p[0] == '\n' && p[1] == '\n')               return p + 2;
        if (p[0] == '\r' && p[1] == '\n' &&
            p[2] == '\r' && p[3] == '\n')               return p + 4;
        if ((size_t)(p - raw) > 8192) break;
        p++;
    }
    return raw;
}

email_t *new_email(char *raw)
{
    email_t *tmp = malloc(sizeof(email_t));
    if (!tmp)
        return NULL;
    tmp->body                = raw;
    tmp->last_index          = 0;
    tmp->exhausted           = 0;
    tmp->yield_if_empty_chain = 0;
    char *body = skip_mime_headers(raw);
    if (body != raw) {
        tmp->body                = body;
        tmp->yield_if_empty_chain = 1;
    }
    return tmp;
}

long get_file_size(FILE *fd)
{
    fseek(fd, 0, SEEK_END);
    long size = ftell(fd);
    rewind(fd);
    return size;
}

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <email_file>\n", av[0]);
        return 1;
    }
    FILE *fd = fopen(av[1], "rb");
    if (!fd) {
        perror(av[1]);
        return 1;
    }
    long size = get_file_size(fd);
    char *raw = malloc(size + 1);
    if (!raw) {
        fclose(fd);
        return 1;
    }
    raw[size] = '\0';
    email_t *email = new_email(raw);
    if (!email) {
        free(raw);
        fclose(fd);
        return 1;
    }
    fread(raw, size, 1, fd);
    fclose(fd);
    get_next_val(email);
    get_next_val(email);
    get_next_val(email);
    free(email);
    free(raw);
    return 0;
}
