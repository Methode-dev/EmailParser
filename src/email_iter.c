#include "email_iter.h"
#include "mime.h"
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static int get_index_sep(char *body) {
    /*
     * body: NUL-terminated string to search
     *
     * description:
     * compiles SEPARATOR_REGEX and searches body for the first match
     * (From:, De:, etc.). the regex is freed after each call.
     *
     * return: byte offset of match start, or -1 if no match
     */
    regex_t regex;
    regmatch_t match;
    int ret;

    if (regcomp(&regex, SEPARATOR_REGEX, REG_EXTENDED) != 0)
        return -1;
    ret = regexec(&regex, body, 1, &match, 0);
    regfree(&regex);
    if (ret == REG_NOMATCH)
        return -1;
    return (int)match.rm_so;
}

email_t *new_email(char *raw) {
    /*
     * raw: malloc'd email buffer owned by the caller
     *
     * description:
     * allocates and zero-initialises an email_t iterator struct.
     * the body pointer is set to raw; caller must keep raw alive
     * for the lifetime of the returned struct.
     *
     * return: heap-allocated email_t, or NULL on allocation failure
     */
    email_t *e;

    e = malloc(sizeof(email_t));
    if (!e)
        return NULL;
    e->body = raw;
    e->last_index = 0;
    e->exhausted = 0;
    e->yield_if_empty_chain = 0;
    return e;
}

bool get_next_val(email_t *email) {
    /*
     * email: iterator state for the current email chain
     *
     * description:
     * advances the iterator to the next segment. the body pointer is
     * moved forward by last_index, the next separator is searched, and
     * a NUL is written two bytes before it to terminate the current
     * segment. stops early when a MIME HTML part boundary is detected,
     * yielding the last plain-text segment before returning false on
     * the subsequent call.
     *
     * return: true if a new segment is available, false when exhausted
     */
    int had_sep;
    int idx;

    if (email->exhausted)
        return false;
    had_sep = (email->last_index != 0);
    email->body += email->last_index;
    if (!*email->body)
        return false;
    idx = get_index_sep(email->body + 1);
    if (idx < 0) {
        if (!had_sep && !email->yield_if_empty_chain)
            return false;
        email->exhausted = 1;
        return true;
    }
    if (has_html_mime_part(email->body + 1, idx)) {
        if (idx >= 1)
            email->body[idx - 1] = '\0';
        email->exhausted = 1;
        return true;
    }
    email->last_index = idx + 1;
    if (email->last_index >= 2)
        email->body[email->last_index - 2] = '\0';
    return true;
}
