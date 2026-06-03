# emailparser — developer internals

This document walks through the internal architecture, data structures, and
call chains of the library.
It is highly recommanded you refer to it before submitting contributions.
It has been generated with Claude Code.
The code itself has been partially generated with Claude Code, especially the guards and error management as it turns out I find it highly efficient at those kind of tasks.

---

## Table of contents

1. [Repository map](#1-repository-map)
2. [Core data structures](#2-core-data-structures)
3. [The iterator state machine](#3-the-iterator-state-machine)
4. [Call chains](#4-call-chains)
   - [Email() construction](#emailobject-construction-email_init)
   - [next(email) — one segment](#nextemail--one-segment-email_next)
   - [parse_headers()](#parse_headers)
   - [extract_body()](#extract_body)
   - [find_signature() / strip_signature()](#find_signature--strip_signature)
5. [MIME handling in depth](#5-mime-handling-in-depth)
6. [HTML handling in depth](#6-html-handling-in-depth)
7. [Python/C boundary conventions](#7-pythonc-boundary-conventions)
8. [Adding a feature — checklist](#8-adding-a-feature--checklist)

---

## 1. Repository map

```
email.h              ← shared C struct (email_t) — included by everyone
emailparser.c        ← Python type + module init only, no parsing logic
setup.py             ← compiles emailparser.c + all src/*.c with -I. -Isrc
src/
  buf.h              ← header-only: strbuf_t + sb_push (inline)
  mime.h / mime.c    ← decode_qp, skip_mime_headers, has_html_mime_part
  html.h / html.c    ← walk_text, segment_to_text, html_to_plain_c
                        + looks_like_html (static inline in html.h)
  standalone.h / .c  ← collect_style_nodes, extract_css, wrap_standalone
  email_iter.h / .c  ← SEPARATOR_REGEX macros, new_email, get_next_val
                        + get_index_sep (static, regex-based)
                        + has_html_mime_part (static, MIME boundary check)
  headers.h / .c     ← canonical_key, split_recipients (static),
                        flush_field (static), py_parse_headers
  body.h / .c        ← find_body_start, py_extract_body
  signature.h / .c   ← is_signature_line (static), find_sig_text_node (static),
                        find_sig_in_html (static), find_sig_in_plain (static),
                        py_find_signature, py_strip_signature
```

**Visibility rules:**
- `static` = used only within its own `.c` file.
- Non-`static` functions are declared in the matching `.h` and may be called
  by other translation units.
- `static inline` in a header = compiled into every `.c` that includes it
  (`sb_push`, `looks_like_html`).

---

## 2. Core data structures

### `email_t` (`email.h`)

The iterator state for one email chain. Lives on the heap, owned by
`EmailObject`.

```c
typedef struct email_s {
    int   last_index;           // bytes to advance body on the next call
    int   exhausted;            // 1 → stop iterating immediately
    int   yield_if_empty_chain; // 1 → yield body even with no separator
    char *body;                 // pointer into the raw buffer (not owned)
} email_t;
```

`body` is a sliding pointer into the `raw` buffer owned by `EmailObject`.
It is **never freed** directly — freeing `raw` (via `EmailObject.raw`)
covers it.

### `EmailObject` (`emailparser.c`)

The Python-facing wrapper, allocated by the CPython heap.

```c
typedef struct {
    PyObject_HEAD           // CPython bookkeeping (ref count, type pointer)
    email_t *email;         // iterator state
    char    *raw;           // malloc'd copy of the entire source
    char    *css;           // extracted <style> CSS (NULL if not standalone)
    size_t   css_len;
    size_t   outer_hdr_len; // bytes of raw[] that are outer MIME headers
    int      exhausted;     // mirrors email->exhausted for Python layer
    int      plain_text;
    int      standalone;
    int      strip_headers;
} EmailObject;
```

### `strbuf_t` (`src/buf.h`)

A dynamically growing byte buffer. Used wherever a C string must be built
incrementally (HTML text extraction, CSS collection, header value
accumulation).

```c
typedef struct { char *buf; size_t len, cap; } strbuf_t;
```

`sb_push(sb, s, n)` appends `n` bytes of `s`, doubling capacity as needed.
Always NUL-terminates. Returns -1 on allocation failure.

Caller initialises with `{NULL, 0, 0}` and must `free(sb.buf)` when done.

---

## 3. The iterator state machine

`get_next_val()` in `src/email_iter.c` drives the whole iteration. Each
call moves the `body` pointer forward and null-terminates the previous
segment in-place, so the caller can use `email->body` as a C string without
copying.

```
Initial state:
  raw = "Date: …\nFrom: A\n…\nFrom: B\n…\nFrom: C\n…EOF\0"
  body → raw[0]
  last_index = 0
  exhausted  = 0

Call 1 (advance by last_index=0, search from body+1):
  finds "From: A" at idx=N
  writes '\0' at body[N-1]   ← terminates segment 0
  last_index = N+1
  returns true → yields body (= raw[0])

Call 2 (advance by N+1, body now → "From: A…"):
  finds "From: B" at idx=M
  writes '\0' at body[M-1]   ← terminates segment 1
  last_index = M+1
  returns true → yields body (= "From: A…")

Call 3 (advance, body → "From: B…"):
  finds "From: C" → but gap contains MIME boundary?
    → writes '\0' at body[idx-1], sets exhausted=1
    → returns true → yields body (= "From: B…")

Call 4:
  exhausted=1 → returns false immediately
```

**Edge cases handled inside `get_next_val`:**

| Condition | Behaviour |
|---|---|
| `exhausted == 1` | Returns `false` immediately |
| `*body == '\0'` | Returns `false` (nothing left in buffer) |
| `idx < 0` (no more separators) | Yields tail if `had_sep` or `yield_if_empty_chain`; otherwise returns `false` |
| MIME HTML boundary in gap | Null-terminates at boundary, sets `exhausted=1`, yields last segment |

**`yield_if_empty_chain`** is set when `skip_mime_headers()` advanced the
body pointer past outer MIME headers. Without it, a file with a single email
and no reply chain would produce zero segments (no separator ever found, so
`had_sep` would be false).

---

## 4. Call chains

### `EmailObject` construction (`Email_init`)

```
Email("file.html", plain_text=True, standalone=False, strip_headers=False)
  │
  ├─ PyArg_ParseTupleAndKeywords()        parse Python args
  │
  ├─ fopen() / fread()  OR  strdup()      load source into raw[]
  │
  ├─ new_email(raw)                       src/email_iter.c
  │    allocates email_t, sets body=raw, all flags=0
  │
  ├─ skip_mime_headers(raw)               src/mime.c
  │    scans raw for first \n\n or \r\n\r\n within 8 KB
  │    if found and raw[0] != '<':
  │      email->body = body_start
  │      email->yield_if_empty_chain = 1
  │      self->outer_hdr_len = (body_start - raw)
  │
  └─ extract_css(raw, size)               src/standalone.c  [only if standalone]
       htmlReadMemory()                   libxml2
       collect_style_nodes()              recursive DOM walk
       returns malloc'd CSS string
```

### `next(email)` — one segment (`Email_next`)

```
Email.__next__
  │
  ├─ get_next_val(email)                  src/email_iter.c
  │    │
  │    ├─ email->body += last_index       slide pointer forward
  │    ├─ get_index_sep(body+1)           static, regex search
  │    │    regcomp(SEPARATOR_REGEX)
  │    │    regexec() → rm_so
  │    │    regfree()
  │    │
  │    ├─ [if idx >= 0] has_html_mime_part(body+1, idx)
  │    │    scans for "--BOUNDARY\nContent-type: text/html"
  │    │    → if found: body[idx-1]='\0', exhausted=1, return true
  │    │
  │    └─ body[last_index-2] = '\0'       terminate current segment
  │
  ├─ [if strip_headers] find_body_start(body, len)   src/body.c
  │    skips leading \n/\r
  │    checks first line has recognised header field (canonical_key)
  │    scans to first blank line → returns pointer to body text
  │
  ├─ [if plain_text] segment_to_text(body)   src/html.c
  │    htmlReadMemory()
  │    walk_text() → sb_push text nodes, \n at block elements
  │    PyUnicode_FromStringAndSize()
  │
  ├─ [if standalone] wrap_standalone(css, css_len, body, len)  src/standalone.c
  │    looks_like_html(body, len)         static inline, src/html.h
  │    [if not HTML] decode_qp()          src/mime.c
  │    assembles HEAD+css+MID+open+body+close+POST into malloc'd buf
  │    PyUnicode_FromStringAndSize()
  │
  └─ [default] PyUnicode_FromString(body)
```

### `parse_headers()`

```
py_parse_headers(module, args)           src/headers.c
  │
  ├─ looks_like_html(text, len)          static inline, src/html.h
  │    scans first 512 bytes for </tag> or <tag>
  │
  ├─ [if HTML] html_to_plain_c(text)     src/html.c
  │    htmlReadMemory()
  │    walk_text() → strbuf_t
  │    returns malloc'd plain-text string
  │
  ├─ decode_qp(plain, plain_len)         src/mime.c
  │    decodes =XX and =\n soft breaks
  │
  ├─ skip leading \n/\r in decoded text
  │
  └─ line-by-line scan until blank line:
       colon = memchr(line, ':', llen)
       canonical_key(fname, flen)        looks up EN/FR field name
         tolower ASCII bytes, keeps non-ASCII UTF-8 as-is
         strcmp against MAP[] table
       sb_push(&cur_val, …)              accumulate field value
       flush_field(dict, key, &cur_val)
         [list fields] split_recipients()
           angle-bracket + quote aware splitting on , and ;
         [string fields] PyDict_SetItemString (first occurrence only)
```

### `extract_body()`

```
py_extract_body(module, args)            src/body.c
  │
  └─ find_body_start(text, len)
       skip leading \n/\r
       check first non-blank line for ':' → trim → canonical_key()
       if not a recognised field: return text unchanged
       scan forward line by line until blank line
       return pointer to first byte after blank line
```

### `find_signature()` / `strip_signature()`

```
py_find_signature(module, args)          src/signature.c
  │
  ├─ scan first 512 bytes for '<'
  │    [if HTML] find_sig_in_html(text, len)
  │      htmlReadMemory()
  │      find_sig_text_node(root)        recursive DOM walk
  │        for each XML_TEXT_NODE:
  │          strip whitespace
  │          is_signature_line(s, len)
  │            checks "--" delimiter
  │            tolower → strncmp against CLOSING_PATTERNS[]
  │        returns malloc'd copy of first matching text
  │      memmem(html, html_len, sig_text, …)  locate in original bytes
  │      returns byte offset
  │
  ├─ [fallback] find_sig_in_plain(text, len)
  │    line-by-line scan
  │    is_signature_line() on each line
  │    returns byte offset of first match
  │
  └─ convert byte offset → char offset
       PyUnicode_DecodeUTF8(text, byte_off)
       PyUnicode_GetLength(prefix)

py_strip_signature(module, args)
  └─ py_find_signature()  →  idx
     PySequence_GetSlice(full, 0, idx)
```

---

## 5. MIME handling in depth

### Why `skip_mime_headers` exists

Raw MIME emails (`.eml`) start with outer headers:

```
Date: Wed, 11 Jun 2025 13:25:14 +0200
Subject: MV RANGER - Schengen Visa
From: "D9A" <d9a@marlowgroup.com>
To: docs <docs@interportfrance.fr>
MIME-Version: 1.0
Content-Type: multipart/alternative; boundary="B_123"

--B_123
Content-Type: text/plain; charset="UTF-8"
…email chain body…
```

Without skipping, the outer `From:` at line 3 would be treated as the first
chain separator, producing a useless 102-byte "segment 0" (just `Date:` and
`Subject:`).

`skip_mime_headers` advances `email->body` to the byte after the first
`\n\n` (or `\r\n\r\n`) within the first 8 KB, but **only if the file does
not start with `<`** (HTML files have no outer header block).

The skipped bytes are preserved: `outer_hdr_len` records their length so
`Email_get_outer_headers` can parse them later via `py_parse_headers`.

### How MIME deduplication works (`has_html_mime_part`)

`multipart/alternative` emails contain the same email chain twice: once as
`text/plain` (with `=20` quoted-printable) and once as `text/html`. Without
deduplication, the iterator would yield ~60 segments instead of ~30.

`has_html_mime_part` is called inside `get_next_val` every time a separator
is found, on the **gap between the current and next separator**. It scans
that gap for:

```
--BOUNDARY\n
Content-type: text/html
```

If found, `get_next_val` null-terminates at the boundary, sets
`exhausted=1`, and returns `true` one final time — yielding the last
plain-text segment before the HTML duplicate would begin.

Opening boundaries (`--BOUNDARY` without the `text/html` line) are ignored,
so inline image boundaries inside the HTML chain do not trigger a false stop.

### Quoted-printable decoding (`decode_qp`)

`=XX` sequences are decoded to their byte values. `=\n` and `=\r\n` (soft
line breaks) are removed. The output buffer is always `≤ in_len` bytes, so
`malloc(in_len + 1)` is always sufficient.

`decode_qp` is called:
1. In `wrap_standalone` — for plain-text segments before `<pre>` wrapping.
2. In `py_parse_headers` — before scanning header lines.

---

## 6. HTML handling in depth

### `looks_like_html` (inline, `src/html.h`)

Used in three places to decide whether to take the HTML or plain-text code
path:

```c
// Scans first 512 bytes for </tag> or <word> / <word attr>
// NOT triggered by bare angle brackets in email addresses (<user@host>)
// because those have non-alpha chars immediately after '<'
static inline int looks_like_html(const char *text, size_t len);
```

Detection rule: `<` followed by `/` → HTML. `<` followed by only
alpha characters and then `>`, ` `, or `/` → HTML. Anything else → not HTML.

This correctly handles `<docs@interportfrance.fr>` (non-alpha `d`…`@`) while
correctly detecting `<div>`, `<br/>`, `</span>` etc.

### `walk_text` (recursive, `src/html.c`)

Traverses the libxml2 DOM depth-first:

- `XML_TEXT_NODE` → `sb_push` content verbatim.
- `XML_ELEMENT_NODE` that is a block tag (`p`, `div`, `br`, `tr`, `li`,
  `h1`–`h6`) → `sb_push("\n", 1)` before descending into children.

All other node types (comments, CDATA, attributes) are silently skipped.

### Two wrappers around `walk_text`

| Function | Returns | Used by |
|---|---|---|
| `segment_to_text` | `PyObject *` (Python string) | `Email_next` (plain_text mode) |
| `html_to_plain_c` | `char *` (malloc'd C string) | `py_parse_headers`, `find_sig_in_html` |

Both call `htmlReadMemory` then `walk_text` then `xmlFreeDoc`. The only
difference is the return type and ownership.

### `extract_css` / `collect_style_nodes` (`src/standalone.c`)

`collect_style_nodes` recursively walks the DOM looking for
`XML_ELEMENT_NODE` named `"style"`, extracts their content with
`xmlNodeGetContent`, and appends it to a `strbuf_t`. This handles `<style>`
blocks anywhere in the document (head or body).

`extract_css` is called once at `Email_init` time (when `standalone=True`)
and the result is stored in `self->css`. Every subsequent segment reuses the
same CSS blob.

---

## 7. Python/C boundary conventions

### Function signatures

Every module-level Python function follows:

```c
static PyObject *py_foo(PyObject *module, PyObject *args)
{
    const char  *text;
    Py_ssize_t  text_len;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len))
        return NULL;
    …
    return result;   /* new reference */
}
```

`"s#"` gives UTF-8 bytes + length. The byte offset returned by C-level
functions must be converted to a **character offset** before returning to
Python, because Python `str` slicing is character-based:

```c
PyObject *prefix = PyUnicode_DecodeUTF8(text, byte_off, "replace");
Py_ssize_t char_off = PyUnicode_GetLength(prefix);
Py_DECREF(prefix);
return PyLong_FromSsize_t(char_off);
```

### Ownership rules

| Situation | Rule |
|---|---|
| `PyObject *` returned to Python | New reference — Python's GC takes over |
| `PyObject *` stored internally | Must `Py_INCREF` when stored, `Py_DECREF` when released |
| `malloc`-allocated C strings | Caller frees unless documented otherwise |
| `email->body` pointer | Points into `self->raw` — do **not** free separately |
| `strbuf_t.buf` | Always `free(sb.buf)` after use; safe to call on `NULL` |

### Re-init safety

`Email_init` is called when `Email(…)` is created, but Python allows calling
`obj.__init__(…)` again on an existing object. The function handles this by
freeing all previous state at the top before doing anything else:

```c
free(self->email); self->email = NULL;
free(self->raw);   self->raw   = NULL;
free(self->css);   self->css   = NULL;
```

`PyType_GenericNew` zero-initialises the struct, so on first construction
`free(NULL)` is a no-op.

---

## 8. Adding a feature — checklist

### New standalone C function (e.g. `extract_date`)

1. **Implement** in the most appropriate `src/*.c` file.
2. **Declare** in the matching `src/*.h`.
3. **Write a Python wrapper** `py_extract_date` in the same `.c` file.
4. **Register** in `emailparser_methods[]` in `emailparser.c`.
5. **Add tests** in `test_emailparser.py` and/or `functional_tests.py`.

### New `Email()` keyword argument (e.g. `decode_qp=True`)

1. Add a field to `EmailObject` in `emailparser.c`.
2. Reset it to `0` in the re-init block at the top of `Email_init`.
3. Add it to `kwlist[]` and `PyArg_ParseTupleAndKeywords` format string
   (`"O|pppp"` → one more `p`).
4. Use it in `Email_next` (or `Email_init` if it affects setup).
5. Update the `tp_doc` string.

### New separator language (e.g. German `Von:`)

Edit `SEPARATOR_REGEX_STA_ALL` in `src/email_iter.h`:

```c
#define SEPARATOR_REGEX_STA_ALL "(De|From|Von) ?(&nbsp;:|:) ?"
```

Also add the field name to `canonical_key`'s MAP in `src/headers.c` if you
want `parse_headers` to recognise the corresponding header fields.

### New signature closing phrase

Add a lowercase string to `CLOSING_PATTERNS[]` in `src/signature.c`:

```c
static const char *CLOSING_PATTERNS[] = {
    …
    "mit freundlichen grüßen",   /* German */
    NULL
};
```

Non-ASCII UTF-8 bytes are matched byte-for-byte after ASCII lowercasing
(`tolower` is applied only to bytes `< 0x80`).
