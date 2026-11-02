#!/usr/bin/env python3
"""0017 layering: os/ must not include any header from tests/.

docs/specs/0017-console-and-os-layout.md:22:

    依赖方向是单向的：`os/` 不得包含 `tests/` 下的任何头；`tests/` 可以包含 `os/`。

The OS is the product and the suite is a client of it: os/ never includes
tests/, while tests/ may include os/. Nothing checked the forward direction, so
one `#include "tests/helpers.h"` in os/ would invert the dependency while every
test stayed green, because the header exists on disk.

Scope: literal #include directives in os/**/*.{c,h,S,s}. A comment or string
literal that merely names a tests/ header is text, not a dependency, so comments
and string/char literals are classified before matching; a comment *inside* a
directive (#/**/include, #include/**/<x>, or one preceding the #) leaves a valid
directive and is still checked. The operand is judged by the `tests/` path
segment it names, so "tests/x.h", "../tests/x.h", "./tests/x.h",
"sub/../../tests/x.h" and backslash spellings all count.

Limitations: the path segment match is case-sensitive, so `Tests/` and `TESTS/`
are not captured by it even on a host filesystem that ignores case; those
spellings fall outside this check, which narrows what the gate detects and does
not make them permitted. Single-line directives only -- a backslash-continued
operand and a directive split across lines are not assembled, so they are not
resolved. There is no preprocessor and no dependency graph, so macro-built
operands, --include, extra -I search paths and headers reached indirectly are
invisible, and a path is judged by its written form rather than by filesystem
resolution.

Diagnostics are pure ASCII: a non-ASCII path or line is backslash-escaped, so a
cp1252 console cannot fail the gate. Output separates three outcomes: violation
(exit 1), unusable scan input (exit 2, its own marker), clean pass (exit 0). A
missing os/, an os/ with no supported source, and an unreadable source are input
failures, never a silent pass, and never the external-dependency skip code 77.
"""
import argparse
import re
import sys
from pathlib import Path

SUFFIXES = (".c", ".h", ".S", ".s")
# The operand may be quoted or angle-bracketed and may start with ./ or ../.
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](?P<path>[^>"]+)[>"]', re.IGNORECASE)
# Case-sensitive on purpose: this pattern matches the lowercase `tests/`
# segment only, so `Tests/` and `TESTS/` are not captured even when the host
# filesystem ignores case. That is a limit of this check, not a permission.
TESTS_SEGMENT = re.compile(r'(?:^|/)tests/')
FAIL = ":FAIL: 0017_os_must_not_include_tests:"
INPUT = ":FAIL: 0017_layering_scan_input:"


def ascii_text(value):
    """Backslash-escape anything outside ASCII instead of emitting raw bytes."""
    return str(value).encode("ascii", "backslashreplace").decode("ascii")


def blank_comments(text):
    """Blank comments to spaces, preserving length and newlines.

    String and char literals are left intact: a `/*` inside one opens nothing,
    and the operand of a directive lives in a literal. Blanking whole comments
    means `/* lead */ #include "tests/x.h"` and `#/**/include` both reduce to a
    plain directive, while a commented-out include reduces to whitespace.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text[i:i + 2] == "//":
            end = text.find("\n", i)
            end = n if end < 0 else end
            for k in range(i, end):
                out[k] = " "
            i = end
        elif text[i:i + 2] == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if text[k] != "\n":
                    out[k] = " "
            i = end
        elif text[i] in "\"'":
            quote = text[i]
            i += 1
            while i < n and text[i] not in (quote, "\n"):
                i += 2 if text[i] == "\\" else 1
            i += 1 if i < n and text[i] == quote else 0
        else:
            i += 1
    return "".join(out)


def scan_text(text, relative):
    hits = []
    for lineno, line in enumerate(blank_comments(text).split("\n"), 1):
        match = INCLUDE.match(line)
        if not match:
            continue
        included = match.group("path").replace("\\", "/")
        if TESTS_SEGMENT.search(included):
            hits.append("%s:%d: %s" % (relative, lineno,
                                       ascii_text(line.strip()[:100])))
    return hits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    os_dir = args.source / "os"
    if not os_dir.is_dir():
        print("%s os/ is missing under %s -- nothing to scan"
              % (INPUT, ascii_text(args.source)), flush=True)
        return 2

    files = sorted(p for p in os_dir.rglob("*")
                   if p.is_file() and p.suffix in SUFFIXES)
    if not files:
        print("%s no %s source under os/ -- nothing to scan"
              % (INPUT, "/".join(SUFFIXES)), flush=True)
        return 2

    scanned = 0
    hits = []
    unreadable = []
    for path in files:
        relative = ascii_text(path.relative_to(args.source).as_posix())
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            unreadable.append("%s: %s" % (relative, ascii_text(error)))
            continue
        scanned += 1
        hits.extend(scan_text(text, relative))

    print("0017 layering: scanned %d files in os/" % scanned, flush=True)
    if unreadable:
        for entry in unreadable:
            print(INPUT + " cannot read source: " + entry, flush=True)
        return 2
    if hits:
        for hit in hits:
            print(FAIL + " " + hit, flush=True)
        return 1
    print("0017 PASS: os/ includes only its own headers", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
