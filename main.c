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
#endif


int get_index_sep(char *email)
{
    regex_t regex;
    regmatch_t match;

    int reti = regcomp(&regex, SEPARATOR_REGEX_STA_ALL, REG_EXTENDED);
    reti = regexec(&regex, email, 1, &match, 0);
    return match.rm_so;
}

int find_char(char tok, char *str)
{
    int i = 0;
    for (; str[i] && str[i] != tok; i++);
    return i;
}

void get_next_val(email_t *email)
{
    email->body = &email->body[email->last_index];
    email->last_index = get_index_sep(email->body++);
    email->body[email->last_index - 2] = '\0';
}

email_t *new_email(char *raw)
{
    email_t *tmp = malloc(sizeof(email_t));
    tmp->body = raw;
    tmp->last_index = 0;
    return tmp;
}

int get_file_size(FILE *fd)
{
    int size;

    fseek(fd, 0, SEEK_END);
    size = ftell(fd);
    rewind(fd);
    return size;
}

int main(int ac, char **av)
{
    FILE *fd = fopen(av[1], "rb");
    int size = get_file_size(fd);
    char *raw = malloc(sizeof(char) * size + 1);
    email_t *email = new_email(raw);

    fread(raw, size, 1, fd);
    get_next_val(email);
    get_next_val(email);
    printf("%s\n", email->body);
    get_next_val(email);
    return 0;
}