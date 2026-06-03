#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "email.h"
#include "src/email_iter.h"
#include "src/mime.h"

static long get_file_size(FILE *fd) {
    fseek(fd, 0, SEEK_END);
    long size = ftell(fd);
    rewind(fd);
    return size;
}

int main(int ac, char **av) {
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
    fread(raw, size, 1, fd);
    fclose(fd);

    email_t *email = new_email(raw);
    if (!email) {
        free(raw);
        return 1;
    }

    char *body = skip_mime_headers(raw);
    if (body != raw) {
        email->body = body;
        email->yield_if_empty_chain = 1;
    }

    get_next_val(email);
    get_next_val(email);
    get_next_val(email);
    free(email);
    free(raw);
    return 0;
}
