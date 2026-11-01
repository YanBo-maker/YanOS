#!/usr/bin/env python3
"""0014 (layering): src/transport.c must not contain upper-layer vocabulary.

docs/specs/0014-host-transport-channel.md:100:

    通道不解释任何字节：它不知道载荷是控制台字符、块请求还是知识服务 RPC。
    `src/transport.c` 中不得出现上层词汇（`note`、`markdown`、`rpc`、`block` 等）；
    Host 侧适配不得解析知识格式。

The file is currently clean, so the requirement holds -- but nothing checked it.
A single `if (strncmp(payload, "note", 4) == 0)` would break the layering while
every existing test stayed green: the channel still moves the same bytes, it
just starts knowing what they mean.

This is a source scan rather than a behavioural test for the same reason as
0036-R8-AC3: the claim is about what the source does not contain, and a channel
that understands the payload produces identical transport behaviour.

Design notes:

1. Case-insensitive, with alphanumeric boundaries and an optional plural `s`. The
   spec names words, but in C the vocabulary arrives as identifiers, so the
   pattern has to see `sniff_rpc`, `block_count` and `blocks` while leaving
   `blocking`, `notebook` and `denote` alone. See the PATTERN comment for why `\b`
   fails at this and why `_` must count as a separator.

2. Only src/transport.c is scanned, exactly as the spec scopes it. Other files
   in src/ describe buses, RAM and registers; `block` and `note` are legitimate
   there, and scanning them would make the gate fail for correct code.

3. Line comments are NOT exempted. The spec forbids the vocabulary in the file,
   not in executable statements only; a comment like
   `/* payload is a block request */` documents exactly the coupling the
   layering rule exists to prevent.
"""
import argparse
import re
import sys
from pathlib import Path

TARGET = "src/transport.c"

# The spec's list, plus `markdown`, which it also names.
FORBIDDEN = ("note", "markdown", "rpc", "block")

# Boundaries are "not alphanumeric" rather than `\b`, and that difference decides
# whether this gate works. In Python's `\b`, `_` is a word character, so `\brpc\b`
# does NOT match `yan_transport_sniff_rpc`, `\bblock\b` does NOT match
# `block_count`, and the plural `blocks` escapes too. In C those are precisely the
# forms the vocabulary takes: an identifier naming the upper layer. Measured on
# 13 cases (7 violations, 6 legitimate lookalikes), `\b` got 5 wrong and this
# pattern got 0.
#
# Requiring a non-alphanumeric neighbour still keeps `_` usable as a separator, so
# `rpc_envelope` and `sniff_rpc` are both caught, while `blocking`, `notebook`,
# `denote` and `footnote` are left alone -- their forbidden fragment is glued to
# letters, not separated by `_`.
#
# An optional trailing `s` covers the plural, which is how `block` most often
# appears in code (`blocks[4]`, `note_count` is caught by the separator rule).
PATTERN = re.compile(
    r"(?<![A-Za-z0-9])(?:" + "|".join(re.escape(w) for w in FORBIDDEN) + r")s?"
    r"(?![A-Za-z0-9])",
    re.IGNORECASE,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    path = args.source / TARGET
    if not path.is_file():
        print(":FAIL: 0014_transport_no_upper_vocabulary: %s is missing"
              % TARGET, flush=True)
        return 1

    text = path.read_text(errors="replace")
    hits = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for match in PATTERN.finditer(line):
            hits.append("%s:%d: '%s' in %s"
                        % (TARGET, lineno, match.group(0), line.strip()[:90]))
            break

    print("0014 transport vocabulary: scanned %s (%d bytes) for %s"
          % (TARGET, len(text), "/".join(FORBIDDEN)), flush=True)
    if hits:
        for hit in hits:
            print(":FAIL: 0014_transport_no_upper_vocabulary: " + hit,
                  flush=True)
        return 1
    print("0014 PASS: the transport channel knows nothing about what it carries",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
