# emailparser

A C extension for Python that splits email reply chains into individual
segments and extracts structured data from each one.

This README has been generated with Claude Code.
The code itself has been partially generated with Claude Code, especially the guards and error management as it turns out I find it highly efficient at those kind of tasks. The core logic is man-made but there is no strong re-reads of the modifications Claude made so there might be errors in there.

Please find CONTRIBUTING.md, as it is a deep-dive documentation into the code.

---

## Table of contents

0. [Installation & Quick Start](#0-installation)
1. [Building](#1-building)
2. [Concepts](#2-concepts)
3. [The Email iterator](#3-the-email-iterator)
   - [Input formats](#input-formats)
   - [plain_text](#plain_text)
   - [standalone](#standalone)
   - [strip_headers](#strip_headers)
   - [outer_headers](#outer_headers)
4. [parse_headers](#4-parse_headers)
5. [extract_body](#5-extract_body)
6. [find_signature](#6-find_signature)
7. [strip_signature](#7-strip_signature)
8. [Putting it all together](#8-putting-it-all-together)
9. [Source layout](#9-source-layout)

---
## 0. Installation
```bash
pip install emailparser
```

Quick Start:
```python
from emailparser import Email

chain_mail = Email(open('mail.html', 'r').read())
print(next(chain_mail))
```

See a more detailed usage in [Section 8](#8-putting-it-all-together)

---
## 1. Building

```bash
pip install setuptools
python3 setup.py build_ext --inplace
```

This compiles `emailparser.cpython-*.so` into the project root. All tests
can then be run with:

```bash
python3 -m pytest functional_tests.py test_emailparser.py -v
```

---

## 2. Concepts

An email reply chain looks like this on disk (plain text or HTML):

```
Latest reply body…

From: Alice <alice@example.com>
Sent: Monday, 2 June 2025 10:00 AM
To: Bob
Subject: RE: Project update

Previous reply body…

From: Bob <bob@example.com>
…
```

`emailparser` finds every `From:` / `De:` separator and yields the content
between them as **segments**.  Each segment begins with the separator header
block and ends just before the next one.

Key facts:

- **Segment 0** is the content *before* the first separator — the body of
  the most recent (top) reply.  It has no inline headers of its own.
- **Segments 1, 2, …** each start with their own `From:`/`Sent:`/`To:`/
  `Subject:` block, followed by a blank line and then the reply body.
- For raw MIME emails (`.eml`, `Date: …` at the top of the file), the outer
  header block is skipped automatically and exposed via `outer_headers`.

---

## 3. The Email iterator

```python
import emailparser

for segment in emailparser.Email("path/to/email.html"):
    print(segment)
```

`Email` accepts a file path, a raw string, or bytes.  It is an iterator:
calling `next()` on it returns one segment at a time.

### Input formats

| Input | Behaviour |
|---|---|
| `Email("path/to/file.html")` | Opens and reads the file |
| `Email("<html>…</html>")` | String with no matching file → used as raw content |
| `Email(b"raw bytes")` | Always treated as raw content |

```python
# File path
for seg in emailparser.Email("mail.txt"):
    ...

# Raw string
content = open("mail.txt").read()
for seg in emailparser.Email(content):
    ...

# Bytes
raw = open("mail.txt", "rb").read()
for seg in emailparser.Email(raw):
    ...
```

All three produce identical results.

---

### plain_text

```python
emailparser.Email(source, plain_text=True)
```

Strips HTML tags and decodes entities (`&lt;`, `&nbsp;`, …) via libxml2
before yielding each segment. Block-level elements (`<p>`, `<div>`, `<br>`,
…) are replaced with newlines.

```python
segs = list(emailparser.Email("mail.txt", plain_text=True))

# Without plain_text:
# '<div data-test-id="mailMessageBodyContainer">Dear Ms. De Pedro…'

# With plain_text:
# '\n\n\nDear Ms. De Pedro,\nGood day,\n…'
```

Use `plain_text=True` whenever you want to process the text content rather
than render the HTML.

---

### standalone

```python
emailparser.Email(source, standalone=True)
```

Wraps each segment in a complete, self-contained HTML document so it can be
saved to a file and opened directly in a browser:

```html
<!DOCTYPE html>
<html><head>
  <meta charset="UTF-8">
  <style>/* base CSS + all <style> blocks extracted from the source */</style>
</head>
<body>
  <!-- segment content -->
</body></html>
```

- HTML segments are embedded as-is.
- Plain-text / quoted-printable segments are decoded and wrapped in `<pre>`.
- Ignored when combined with `plain_text=True`.

```python
segs = list(emailparser.Email("test_emails/test2.html", standalone=True))

with open("segment_0.html", "w") as f:
    f.write(segs[0])   # open in browser and it renders correctly
```

---

### strip_headers

```python
emailparser.Email(source, plain_text=True, strip_headers=True)
```

Removes the `From:`/`To:`/`Subject:`/`Date:` header block from the top of
each segment, leaving only the reply body.

- Has **no effect** on segment 0 (which starts with the reply body directly,
  not with a header block).
- Works for both English headers (`From:`, `Sent:`) and French headers
  (`De :`, `Envoyé :`, `À :`).

```python
# Without strip_headers:
# '\nFrom: D9A (Branko Olic)…\nSent: Wednesday…\nTo: docs\n\nHi Abby,…'

# With strip_headers:
# '\nHi Abby,…'
```

You can combine all three flags:

```python
for body in emailparser.Email("chain.html", plain_text=True, strip_headers=True):
    print(body)
```

---

### outer_headers

For raw MIME emails the outer header block (the metadata of the most recent
email) is skipped during iteration but remains accessible as a property:

```python
email = emailparser.Email("test_emails/test2.html")

print(email.outer_headers)
# {
#   "from":    '"D9A (Branko Olic) Marlow CD-D9A" <d9a@marlowgroup.com>',
#   "to":      ['docs <docs@interportfrance.fr>'],
#   "cc":      ['"g2.mnph@marlowgroup.com" <g2.mnph@marlowgroup.com>', …],
#   "bcc":     [],
#   "subject": "MV RANGER - Schengen Visa - OS BALABAT",
#   "date":    "Wed, 11 Jun 2025 13:25:14 +0200"
# }
```

Returns `None` for pure HTML emails (e.g. `mail.txt`) that have no outer
MIME header block.

```python
if email.outer_headers:
    sender = email.outer_headers["from"]
```

---

## 4. parse_headers

```python
emailparser.parse_headers(segment) -> dict
```

Extracts the header fields from any segment string.

**Returns** a dict with these keys — always present, defaults shown:

| Key | Type | Default |
|---|---|---|
| `"from"` | `str \| None` | `None` |
| `"to"` | `list[str]` | `[]` |
| `"cc"` | `list[str]` | `[]` |
| `"bcc"` | `list[str]` | `[]` |
| `"subject"` | `str \| None` | `None` |
| `"date"` | `str \| None` | `None` |
| `"message-id"` | `str \| None` | `None` |
| `"thread-index"` | `str \| None` | `None` |
| `"thread-topic"` | `str \| None` | `None` |

`message-id`, `thread-index`, and `thread-topic` are present in the outermost
MIME header block and are therefore most useful via `email.outer_headers`.
Inner quoted segments rarely carry these fields, so they will return `None`
for most `parse_headers()` calls on segments 1, 2, …

Recognised field names (case-insensitive):

| Language | Fields |
|---|---|
| English | `From`, `To`, `CC`, `BCC`, `Subject`, `Date`, `Sent`, `Reply-To` |
| French  | `De`, `À` / `à`, `Cci`, `Objet`, `Envoyé` |

Handles HTML segments and quoted-printable encoding automatically.

```python
segs = list(emailparser.Email("test_emails/test2.html", plain_text=True))
seg  = segs[3]   # a quoted reply starting with "From: D9A…"

h = emailparser.parse_headers(seg)
# {
#   "from":    "D9A (Branko Olic) Marlow CD-D9A",
#   "to":      ["docs"],
#   "cc":      ["g2.mnph@marlowgroup.com", "Info - INTERPORT", …],
#   "bcc":     [],
#   "subject": "RE: MV RANGER - Schengen Visa - OS BALABAT",
#   "date":    "Wednesday, June 11, 2025 11:49 AM"
# }

print(h["from"])     # "D9A (Branko Olic) Marlow CD-D9A"
print(h["to"])       # ["docs"]
print(h["subject"])  # "RE: MV RANGER - Schengen Visa - OS BALABAT"
```

> **Note:** `parse_headers` on segment 0 returns all `None`/`[]` because
> segment 0 is the latest reply body with no inline header block.
> Use `email.outer_headers` to access the metadata of that first email.

---

## 5. extract_body

```python
emailparser.extract_body(segment) -> str
```

Returns the segment with the header block stripped — symmetric counterpart
to `parse_headers`.

- If the segment starts with a recognised header field, scans to the first
  blank line and returns everything after it.
- If the segment does **not** start with a recognised header (e.g. segment 0),
  it is returned **unchanged**.

```python
seg  = segs[3]   # starts with "From: D9A…\nSent:…\nTo:…\n\nHi Abby,…"
body = emailparser.extract_body(seg)
# '\nHi Abby,\n\nGm,\n\nPls see blw…'

# Segment 0 has no header block — returned as-is
body_0 = emailparser.extract_body(segs[0])
# '\n\nDear Ms. De Pedro,\nGood day,…'
```

Pair it with `parse_headers` to access both parts independently:

```python
headers = emailparser.parse_headers(seg)
body    = emailparser.extract_body(seg)
```

---

## 6. find_signature

```python
emailparser.find_signature(text) -> int
```

Returns the **character index** where the signature block starts, or `-1`
if no signature is found.

Detects:

| Pattern | Example |
|---|---|
| RFC 3676 delimiter | `--` or `-- ` on its own line |
| English closings | `Kind regards`, `Best regards`, `Sincerely`, `Thanks`, … |
| French closings | `Cordialement`, `Bien cordialement`, `Merci`, `Salutations` |

Works on both plain-text and raw HTML segments (libxml2 DOM path for HTML,
line-scan fallback for plain text).

```python
body = emailparser.extract_body(segs[3])

idx = emailparser.find_signature(body)
# 801

if idx >= 0:
    message   = body[:idx]   # reply body without signature
    signature = body[idx:]   # "Kind regards,\n\nBranko Olic…"
```

---

## 7. strip_signature

```python
emailparser.strip_signature(text) -> str
```

Returns `text` with the signature block removed. Shorthand for:

```python
idx = emailparser.find_signature(text)
clean = text[:idx] if idx >= 0 else text
```

Returns the input unchanged if no signature is found.

```python
body  = emailparser.extract_body(segs[3])
clean = emailparser.strip_signature(body)
# reply body text only, no "Kind regards,\n\nBranko Olic…"
```

---

## 8. Putting it all together

Complete example — iterate over a MIME email chain and extract every piece
of structured data:

```python
import emailparser

source = "test_emails/test2.html"
email  = emailparser.Email(source, plain_text=True)

# ── Most recent email (segment 0 has no inline headers) ──────────────────
first_headers = email.outer_headers          # dict or None
first_body    = None                         # collected below

# ── Iterate over all segments ─────────────────────────────────────────────
for i, seg in enumerate(email):
    headers = emailparser.parse_headers(seg)
    body    = emailparser.extract_body(seg)
    clean   = emailparser.strip_signature(body)
    sig_idx = emailparser.find_signature(body)
    sig     = body[sig_idx:] if sig_idx >= 0 else ""

    if i == 0:
        first_body = body   # segment 0 is already just the body

    print(f"── Segment {i} ──────────────────────")
    if i == 0:
        # segment 0: headers come from outer_headers
        if first_headers:
            print(f"  From:    {first_headers['from']}")
            print(f"  Subject: {first_headers['subject']}")
    else:
        print(f"  From:    {headers['from']}")
        print(f"  Date:    {headers['date']}")
        print(f"  Subject: {headers['subject']}")
    print(f"  Body:    {clean[:60].strip()!r}…")
    if sig:
        print(f"  Sig:     {sig[:40].strip()!r}…")
```

---

## 9. Source layout

```
emailparser.c          Python type (EmailObject) and module init
email.h                email_t struct definition
setup.py               build script — compiles all src/*.c files
main.c                 minimal standalone C binary
src/
  buf.h                strbuf_t type and sb_push (header-only)
  mime.h / mime.c      decode_qp · skip_mime_headers · has_html_mime_part
  html.h / html.c      walk_text · segment_to_text · html_to_plain_c
  standalone.h / .c    extract_css · wrap_standalone
  email_iter.h / .c    SEPARATOR_REGEX · new_email · get_next_val
  headers.h / .c       canonical_key · py_parse_headers
  body.h / .c          find_body_start · py_extract_body
  signature.h / .c     py_find_signature · py_strip_signature
test_emailparser.py    unittest suite (mail.txt)
functional_tests.py    pytest parametrised suite (test_emails/)
```

### Compile-time override

The separator regex can be overridden without changing the source:

```bash
python3 setup.py build_ext --inplace \
  build_ext --define SEPARATOR_REGEX='"(From|De) ?:"'
```
