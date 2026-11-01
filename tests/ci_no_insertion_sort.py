#!/usr/bin/env python3
"""0036-R8-AC3 (source half): the production tree must not contain an insertion
sort written as `while (j > 0 ...)`.

Why this is a source scan and not a behavioural test: the AC is a statement about
what the source no longer contains. Swapping the heap sort in os/sort.h for an
insertion sort keeps every observable result identical -- the reviewer measured
`PASS sort_index: 10245 cases` with an insertion sort in place -- so no
result-based test can detect it. The only way to guard the second half of AC3 is
to read the source.

tests/ is deliberately NOT scanned. The reference implementation that AC3's first
half compares against is an insertion sort by definition
(tests/test_sort_index.c:56), so scanning it would make the gate fail on the very
oracle it is supposed to validate against.

Both `while (j > 0` and `for (; j > 0` are rejected. The AC names the `while`
form, but rewriting the loop head as `for` is a mechanical change that would
leave the insertion sort in place while slipping past a `while`-only pattern;
excluding it would make the gate trivially bypassable. A heap sort has no `j`
cursor at all, so neither form appears legitimately.
"""
import argparse
import re
import sys
from pathlib import Path

# Scanned trees: everything that ships. tests/ and build/ are excluded.
SCAN_ROOTS = ("os", "host", "tools", "src", "apps")
SOURCE_SUFFIXES = (".c", ".h", ".S")

# Tolerate any spacing so reformatting cannot hide the shape.
PATTERN = re.compile(r"(?:while|for)\s*\([^)]*\bj\s*>\s*0")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    hits = []
    scanned = 0
    for root_name in SCAN_ROOTS:
        root = args.source / root_name
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            scanned += 1
            text = path.read_text(errors="replace")
            for lineno, line in enumerate(text.splitlines(), 1):
                if PATTERN.search(line):
                    hits.append("%s:%d: %s"
                                % (path.relative_to(args.source), lineno,
                                   line.strip()[:100]))

    print("0036-R8-AC3 scanned=%d files in %s"
          % (scanned, ",".join(SCAN_ROOTS)), flush=True)
    if hits:
        for hit in hits:
            print(":FAIL: 0036_R8_AC3_no_insertion_sort_in_production: " + hit,
                  flush=True)
        return 1
    print("0036-R8-AC3 PASS: no insertion-sort loop head in the production tree",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
