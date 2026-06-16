#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "signature.h"
#include "html.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

/* Multi-word or highly specific phrases — any non-alpha terminator accepted */
static const char *STRONG_PATTERNS[] = {
    "best regards",       "kind regards",       "warm regards",
    "with regards",       "many thanks",        "best wishes",
    "yours sincerely",    "yours faithfully",   "yours truly",
    "cordialement",       "bien cordialement",  "salutations",
    "thanks & best regards", "thanks & kind regards",
    "all the best",       "thank you very much",
    NULL};

/* Short / ambiguous words — must be the whole line or end with comma/period */
static const char *WEAK_PATTERNS[] = {
    "thanks", "cheers", "regards", "sincerely", "merci",
    "best", "yours", "thank you", "thanks again",
    NULL};

/* Lines from the end to search in pass 1 (covers chain email segments) */
#define SIG_CONTEXT_LINES 40
/* Lines from the start to search in pass 2 — fallback for single MIME emails
   whose body text appears near the top, followed by base64 attachments */
#define SIG_FALLBACK_LINES 100
/* Max lines to walk backward from a contact anchor to find block start */
#define MAX_SIG_BLOCK_LINES 15

static const char *find_context_start(const char *text, size_t len); /* fwd */
static int has_body_before(const char *match_start, const char *text_base); /* fwd */

/* Single-letter and common multi-letter contact field labels */
static const char *CONTACT_LABELS[] = {
    "tel:", "t:", "tél:", "phone:", "ph:", "mobile:", "mob:", "m:",
    "cell:", "fax:", "f:", "email:", "e-mail:", "mail:", "e:", "w:",
    "web:", "website:", "adr:", "addr:", "address:", "a:", "dir:",
    "linkedin:", NULL};

static int looks_like_phone(const char *s, size_t len) {
    /*
     * s: trimmed line content
     * len: byte length
     *
     * description:
     * returns 1 if s looks like a phone number: 7-15 digits with only
     * phone-valid separators (spaces, dashes, dots, parentheses) and
     * at least one space/paren/dash OR a leading +. this avoids matching
     * plain dates ("06/04/2026") or reference numbers.
     *
     * return: 1 if phone-like, 0 otherwise
     */
    int digits = 0;
    int has_sep = 0;
    int has_plus = (len > 0 && s[0] == '+');

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (isdigit((unsigned char)c)) {
            digits++;
            continue;
        }
        if (c == ' ' || c == '-' || c == '(' || c == ')') {
            has_sep = 1;
            continue;
        }
        if (c == '.' || c == '\t') {
            has_sep = 1;
            continue;
        }
        if (c == '+' && i == 0)
            continue;
        if (c == '/')
            continue; /* allow but not counted as a separator */
        return 0;
    }
    return (digits >= 7 && digits <= 15 && (has_plus || has_sep));
}

static int looks_like_email(const char *s, size_t len) {
    /*
     * s: trimmed line content (no leading/trailing whitespace)
     * len: byte length
     *
     * description:
     * returns 1 if s is a bare email address: exactly one '@', no spaces,
     * and at least one '.' after the '@'.
     *
     * return: 1 if email-like, 0 otherwise
     */
    int at_idx = -1;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '@') {
            if (at_idx >= 0)
                return 0; /* two @ signs */
            at_idx = (int)i;
            continue;
        }
        if (isspace((unsigned char)s[i]))
            return 0;
    }
    if (at_idx <= 0 || at_idx >= (int)len - 3)
        return 0;
    for (int i = at_idx + 1; i < (int)len; i++)
        if (s[i] == '.')
            return 1;
    return 0;
}

static int is_contact_line(const char *line, size_t len) {
    /*
     * line: one line of text
     * len: byte length
     *
     * description:
     * returns 1 if the line looks like a signature contact-info element:
     *   - URL (http/https/www)
     *   - known contact label prefix (Tel:, E:, T:, Email:, Web:, …)
     *   - bare phone number
     *   - bare email address
     * the contact label alone is sufficient — no need to validate the value,
     * since "E:", "T:", "A:" at the start of a line are highly specific to
     * signature blocks.
     *
     * return: 1 if the line is a contact-info line, 0 otherwise
     */
    const char *s = line;
    size_t n = len;
    char lower[32];
    size_t check;
    int i;

    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0)
        return 0;
    if (n >= 7 && strncasecmp(s, "http://", 7) == 0)
        return 1;
    if (n >= 8 && strncasecmp(s, "https://", 8) == 0)
        return 1;
    if (n >= 4 && strncasecmp(s, "www.", 4) == 0)
        return 1;
    check = n < 31 ? n : 31;
    for (size_t j = 0; j < check; j++)
        lower[j] = (char)tolower((unsigned char)s[j]);
    lower[check] = '\0';
    for (i = 0; CONTACT_LABELS[i]; i++) {
        size_t ll = strlen(CONTACT_LABELS[i]);
        if (strncmp(lower, CONTACT_LABELS[i], ll) == 0)
            return 1;
    }
    if (looks_like_phone(s, n))
        return 1;
    if (looks_like_email(s, n))
        return 1;
    return 0;
}

static const char *block_start_from(const char *anchor_line,
                                     const char *context_start) {
    /*
     * anchor_line: pointer to the start of the contact-anchor line
     * context_start: lower bound — do not walk before this point
     *
     * description:
     * walks backward from anchor_line through consecutive non-blank lines
     * (up to MAX_SIG_BLOCK_LINES) to find the first line of the signature
     * block. stops when a blank line is encountered, which is the standard
     * body/signature separator.
     *
     * return: pointer to the start of the earliest line in the block
     */
    const char *block_start = anchor_line;
    const char *p = anchor_line;
    int lines_back = 0;
    const char *prev;
    const char *prev_line;
    size_t prev_len;
    int blank;

    while (p > context_start && lines_back < MAX_SIG_BLOCK_LINES) {
        prev = p - 1;
        /* p-1 is the \n ending the previous line — skip past it */
        if (prev > context_start && *prev == '\n')
            prev--;
        while (prev > context_start && *prev != '\n')
            prev--;
        prev_line = (*prev == '\n') ? prev + 1 : context_start;
        prev_len = (size_t)((p - 1) - prev_line);
        if (prev_len > 0 && prev_line[prev_len - 1] == '\r')
            prev_len--;
        blank = 1;
        for (size_t i = 0; i < prev_len; i++) {
            if (!isspace((unsigned char)prev_line[i])) {
                blank = 0;
                break;
            }
        }
        if (blank)
            break;
        block_start = prev_line;
        p = prev_line;
        lines_back++;
    }
    return block_start;
}

static Py_ssize_t find_sig_by_contact(const char *text, size_t text_len) {
    /*
     * text: plain-text segment to search
     * text_len: byte length of text
     *
     * description:
     * scans the last SIG_CONTEXT_LINES lines for a contact-info anchor
     * line (phone, email, URL, or labeled field). when found, walks backward
     * to return the position of the first line of the block, not just the
     * anchor. this handles signatures that begin directly with a name/title
     * with no polite closing phrase.
     *
     * return: byte offset of signature block start, or -1 if not found
     */
    const char *context;
    const char *p;
    const char *end;
    const char *ls;
    size_t llen;
    size_t clean;

    context = find_context_start(text, text_len);
    p = context;
    end = text + text_len;
    while (p < end) {
        ls = p;
        while (p < end && *p != '\n')
            p++;
        llen = (size_t)(p - ls);
        clean = (llen > 0 && ls[llen - 1] == '\r') ? llen - 1 : llen;
        if (p < end)
            p++;
        if (is_contact_line(ls, clean)) {
            const char *start = block_start_from(ls, context);
            if (has_body_before(start, text))
                return (Py_ssize_t)(start - text);
        }
    }
    return -1;
}

static int is_separator_line(const char *line, size_t len) {
    /*
     * line: one trimmed line of text
     * len: byte length of line
     *
     * description:
     * returns 1 for visual divider lines that consist entirely of 2 or
     * more identical separator characters from the set {-, _, *, =, ~}.
     * single-char lines are ignored to avoid matching list bullets.
     *
     * return: 1 if line is a visual separator, 0 otherwise
     */
    char first;
    size_t i;

    if (len < 2)
        return 0;
    first = line[0];
    if (first != '-' && first != '_' && first != '*' && first != '=' &&
        first != '~')
        return 0;
    for (i = 1; i < len; i++)
        if (line[i] != first)
            return 0;
    return 1;
}

static int is_signature_line(const char *line, size_t len) {
    /*
     * line: one line of text (without trailing \n)
     * len: byte length of line
     *
     * description:
     * returns 1 if the line is a signature indicator. detection order:
     *   1. visual separator line (---, ___, ***): is_separator_line()
     *   2. RFC 3676 delimiter: exactly "--" or "-- "
     *   3. strong closing phrase: any non-alpha character after the phrase
     *   4. weak closing phrase: must be followed by comma, period, or EOL
     *      only — never by a space-then-word to avoid mid-body false positives
     *      such as "Thanks for your help"
     *
     * return: 1 if line is a signature line, 0 otherwise
     */
    char lower[64];
    size_t copy;
    size_t plen;
    int i;

    if (is_separator_line(line, len))
        return 1;
    if (len >= 2 && line[0] == '-' && line[1] == '-')
        if (len == 2 || (len == 3 && line[2] == ' '))
            return 1;
    copy = len < 63 ? len : 63;
    for (size_t j = 0; j < copy; j++)
        lower[j] = (char)tolower((unsigned char)line[j]);
    lower[copy] = '\0';
    for (i = 0; STRONG_PATTERNS[i]; i++) {
        plen = strlen(STRONG_PATTERNS[i]);
        if (strncmp(lower, STRONG_PATTERNS[i], plen) != 0)
            continue;
        if (copy == plen || !isalpha((unsigned char)lower[plen]))
            return 1;
    }
    for (i = 0; WEAK_PATTERNS[i]; i++) {
        plen = strlen(WEAK_PATTERNS[i]);
        if (strncmp(lower, WEAK_PATTERNS[i], plen) != 0)
            continue;
        if (copy == plen || lower[plen] == ',' || lower[plen] == '.')
            return 1;
    }
    return 0;
}

static const char *find_context_start(const char *text, size_t len) {
    /*
     * text: full plain-text content
     * len: byte length of text
     *
     * description:
     * walks backward from the end of text counting newlines. returns a
     * pointer to the start of the last SIG_CONTEXT_LINES lines so that
     * signature detection is restricted to the tail of the email, avoiding
     * false positives from closing phrases appearing in the body.
     *
     * return: pointer into text at the start of the context window
     */
    const char *p;
    int count;

    p = text + len;
    count = 0;
    while (p > text) {
        p--;
        if (*p == '\n') {
            count++;
            if (count >= SIG_CONTEXT_LINES)
                return p + 1;
        }
    }
    return text;
}

/* Like is_signature_line but only checks separator lines and strong patterns.
   Used in the beginning-fallback pass to keep false-positive risk low. */
static int is_strong_signature_line(const char *line, size_t len) {
    char lower[64];
    size_t copy;
    size_t plen;
    int i;

    if (is_separator_line(line, len))
        return 1;
    if (len >= 2 && line[0] == '-' && line[1] == '-')
        if (len == 2 || (len == 3 && line[2] == ' '))
            return 1;
    copy = len < 63 ? len : 63;
    for (size_t j = 0; j < copy; j++)
        lower[j] = (char)tolower((unsigned char)line[j]);
    lower[copy] = '\0';
    for (i = 0; STRONG_PATTERNS[i]; i++) {
        plen = strlen(STRONG_PATTERNS[i]);
        if (strncmp(lower, STRONG_PATTERNS[i], plen) != 0)
            continue;
        if (copy == plen || !isalpha((unsigned char)lower[plen]))
            return 1;
    }
    return 0;
}

/* Returns 1 if there is at least one non-blank line between text_base and
   match_start, ensuring the signature is not the very first content. */
static int has_body_before(const char *match_start, const char *text_base) {
    const char *p = text_base;
    const char *ls;
    size_t llen;
    size_t i;

    while (p < match_start) {
        ls = p;
        while (p < match_start && *p != '\n')
            p++;
        llen = (size_t)(p - ls);
        if (llen > 0 && ls[llen - 1] == '\r')
            llen--;
        for (i = 0; i < llen; i++) {
            if (!isspace((unsigned char)ls[i]))
                return 1;
        }
        if (p < match_start)
            p++;
    }
    return 0;
}

static Py_ssize_t scan_lines(const char *search_start, const char *end,
                              const char *text_base, int strong_only) {
    const char *p = search_start;
    const char *ls;
    size_t llen;
    size_t clean;

    while (p < end) {
        ls = p;
        while (p < end && *p != '\n')
            p++;
        llen = (size_t)(p - ls);
        clean = (llen > 0 && ls[llen - 1] == '\r') ? llen - 1 : llen;
        if ((strong_only ? is_strong_signature_line(ls, clean)
                         : is_signature_line(ls, clean)) &&
            has_body_before(ls, text_base))
            return (Py_ssize_t)(ls - text_base);
        if (p < end)
            p++;
    }
    return -1;
}

static Py_ssize_t find_sig_in_plain(const char *text, size_t text_len) {
    /*
     * text: plain-text segment to search
     * text_len: byte length of text
     *
     * description:
     * two-pass search to handle both chain email segments and single MIME
     * emails whose signature sits near the top but has base64 attachments
     * below it:
     *
     * pass 1 — last SIG_CONTEXT_LINES lines, full pattern set.
     *   covers chain email segments where the signature is near the end
     *   and avoids false positives from mid-body closing phrases.
     *
     * pass 2 — first SIG_FALLBACK_LINES lines, strong patterns only.
     *   fallback for raw MIME emails where the actual email body (and its
     *   signature) is near the top, followed by base64 attachment data that
     *   would push it outside the pass-1 context window.
     *
     * return: byte offset of signature start relative to text, or -1
     */
    const char *full_end;
    const char *context;
    const char *fallback_end;
    Py_ssize_t off;
    int count;

    full_end = text + text_len;
    context = find_context_start(text, text_len);
    off = scan_lines(context, full_end, text, 0);
    if (off >= 0)
        return off;
    /* Pass 2: scan first SIG_FALLBACK_LINES lines, strong patterns only */
    fallback_end = text;
    count = 0;
    while (fallback_end < full_end && count < SIG_FALLBACK_LINES) {
        if (*fallback_end == '\n')
            count++;
        fallback_end++;
    }
    if (fallback_end <= context) {
        off = scan_lines(text, fallback_end, text, 1);
        if (off >= 0)
            return off;
    }
    /* Pass 3: contact-anchor detection for signatures without a polite closing
       (phone number, email address, URL, or labeled field like "Tel:", "E:") */
    return find_sig_by_contact(text, text_len);
}

static Py_ssize_t find_sig_in_html(const char *html, size_t html_len) {
    /*
     * html: raw HTML segment to search
     * html_len: byte length of html
     *
     * description:
     * extracts plain text from the HTML via html_to_plain_c, then runs
     * the context-aware find_sig_in_plain on it. the signature phrase is
     * then located in the original HTML bytes using memmem so that the
     * returned offset points into the actual HTML source.
     *
     * return: byte offset of signature start in html, or -1 if not found
     */
    char *plain;
    size_t plain_len;
    Py_ssize_t plain_off;
    const char *sig_start;
    const char *sig_end;
    size_t sig_len;
    char *sig_text;
    const char *pos;
    Py_ssize_t off;

    plain = html_to_plain_c(html);
    if (!plain)
        return -1;
    plain_len = strlen(plain);
    plain_off = find_sig_in_plain(plain, plain_len);
    if (plain_off < 0) {
        free(plain);
        return -1;
    }
    sig_start = plain + plain_off;
    sig_end = sig_start;
    while (*sig_end && *sig_end != '\n')
        sig_end++;
    sig_len = (size_t)(sig_end - sig_start);
    while (sig_len > 0 && isspace((unsigned char)sig_start[0])) {
        sig_start++;
        sig_len--;
    }
    while (sig_len > 0 && isspace((unsigned char)sig_start[sig_len - 1]))
        sig_len--;
    if (sig_len == 0) {
        free(plain);
        return -1;
    }
    sig_text = malloc(sig_len + 1);
    if (!sig_text) {
        free(plain);
        return -1;
    }
    memcpy(sig_text, sig_start, sig_len);
    sig_text[sig_len] = '\0';
    free(plain); /* safe: sig_text is an independent copy */
    pos = memmem(html, html_len, sig_text, sig_len);
    off = pos ? (Py_ssize_t)(pos - html) : -1;
    free(sig_text);
    return off;
}

PyObject *py_find_signature(PyObject *module, PyObject *args) {
    /*
     * module: unused Python module argument
     * args: Python tuple containing one string segment
     *
     * description:
     * tries the HTML path first (html_to_plain_c + context-aware scan)
     * if a '<' is found in the first 512 bytes; falls back to plain-text
     * context-aware line scan. converts the resulting byte offset to a
     * character offset for correct Python string slicing with UTF-8.
     *
     * return: Python int — character index of signature start, or -1
     */
    const char *text;
    Py_ssize_t text_len;
    Py_ssize_t byte_off;
    Py_ssize_t i;
    PyObject *prefix;
    Py_ssize_t char_off;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    byte_off = -1;
    for (i = 0; i < text_len && i < 512; i++) {
        if (text[i] == '<') {
            byte_off = find_sig_in_html(text, (size_t)text_len);
            break;
        }
    }
    if (byte_off < 0)
        byte_off = find_sig_in_plain(text, (size_t)text_len);
    if (byte_off < 0)
        return PyLong_FromLong(-1L);
    prefix = PyUnicode_DecodeUTF8(text, byte_off, "replace");
    if (!prefix)
        return NULL;
    char_off = PyUnicode_GetLength(prefix);
    Py_DECREF(prefix);
    return PyLong_FromSsize_t(char_off);
}

PyObject *py_strip_signature(PyObject *module, PyObject *args) {
    /*
     * module: unused Python module argument
     * args: Python tuple containing one string segment
     *
     * description:
     * calls py_find_signature to locate the signature start index, then
     * returns a slice of the input up to (but not including) that index.
     * if no signature is found, the full input string is returned unchanged.
     *
     * return: Python string with signature removed, or original if none found
     */
    const char *text;
    Py_ssize_t text_len;
    PyObject *idx_obj;
    Py_ssize_t idx;
    PyObject *full;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    idx_obj = py_find_signature(module, args);
    if (!idx_obj)
        return NULL;
    idx = PyLong_AsSsize_t(idx_obj);
    Py_DECREF(idx_obj);
    full = PyUnicode_DecodeUTF8(text, text_len, "replace");
    if (!full)
        return NULL;
    if (idx < 0)
        return full;
    result = PySequence_GetSlice(full, 0, idx);
    Py_DECREF(full);
    return result;
}
