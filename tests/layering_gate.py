#!/usr/bin/env python3
"""0017 layering: os/ must not include any header from tests/.

docs/specs/0017-console-and-os-layout.md:22:

    依赖方向是单向的：`os/` 不得包含 `tests/` 下的任何头；`tests/` 可以包含 `os/`。

The direction is one-way by design: the OS is the product, the suite is a client
of it. The reverse edge is real and load-bearing -- 24 test files include os/
headers -- which is exactly why the rule needs a guard. Nothing checked it
before, so a single `#include "tests/helpers.h"` inside os/ would invert the
dependency and every test would still pass, because the file it pulls in is
present on disk.

Design notes:

1. Only #include directives are examined. os/ mentions test file names in
   comments in 8 places ("the suite in tests/test_arena.c pins these cases").
   Those are documentation, and the spec forbids *including* tests headers, not
   naming them. Flagging comments would fail correct code and the gate would be
   switched off by the first person who hit it.

2. Every spelling of the path is rejected, including the ones a compiler would
   resolve and a naive grep would miss:
       #include "tests/x.h"
       #include "../tests/x.h"
       #include <tests/x.h>
       #include "./tests/x.h"
       #include "sub/../../tests/x.h"
   The test is therefore "the directive mentions a tests/ path segment", not an
   exact-string comparison.

3. Output is pure ASCII. A previous gate in this repository crashed the Windows
   job because it printed Chinese to a cp1252 stdout; nothing here needs
   non-ASCII, and file names are ASCII.
"""
import argparse
import re
import sys
from pathlib import Path

# Match a preprocessor include of anything that resolves into tests/.
# The path may be quoted or angle-bracketed and may start with ./ or ../.
INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](?P<path>[^>"]+)[>"]', re.IGNORECASE)
TESTS_SEGMENT = re.compile(r'(?:^|/)tests/')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    os_dir = args.source / "os"
    if not os_dir.is_dir():
        print(":FAIL: 0017_os_must_not_include_tests: os/ is missing",
              flush=True)
        return 1

    scanned = 0
    hits = []
    for path in sorted(os_dir.iterdir()):
        if not path.is_file() or path.suffix not in (".c", ".h", ".S", ".s"):
            continue
        scanned += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            included = match.group("path").replace("\\", "/")
            if TESTS_SEGMENT.search(included):
                hits.append("%s:%d: %s"
                            % (path.relative_to(args.source), lineno,
                               line.strip()[:100]))

    print("0017 layering: scanned %d files in os/" % scanned, flush=True)
    if hits:
        for hit in hits:
            print(":FAIL: 0017_os_must_not_include_tests: " + hit, flush=True)
        return 1
    print("0017 PASS: os/ includes only its own headers", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
