#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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

bool get_next_val(email_t *email)
{
    email->body += email->last_index;
    int idx = get_index_sep(email->body + 1);
    if (idx < 0)
        return false;
    email->last_index = idx + 1;  /* +1: idx is relative to body+1 */
    if (email->last_index >= 2)
        email->body[email->last_index - 2] = '\0';
    return true;
}

email_t *new_email(char *raw)
{
    email_t *tmp = malloc(sizeof(email_t));
    if (!tmp)
        return NULL;
    tmp->body = raw;
    tmp->last_index = 0;
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
