#!/usr/bin/env python3
"""0035-R9-AC1 (README half): the README must still carry the R9/R10 wording.

docs/specs/0035:109 says:

    0035-R9-AC1：description 和 README 中包含上述约束原文，以及 0035-R10 的
    「展示预览、得到闫博明确确认后才提交」约束；README 原有「学习与理解程度仍由
    项目所有者判断」的表述保留不变（以 diff 为准）。

The description half is already guarded by tests/mcp_sdk_check.py, which asserts
each phrase appears in the tool description served over MCP. The README half had
nothing: deleting a sentence from README.md breaks the spec silently, because no
test reads that file.

Why a text scan rather than a behavioural test: the requirement is about the
wording present in a document. Nothing the binaries do can reveal whether a
paragraph was deleted.

Design notes:

1. Substring match, case- and byte-exact. The phrases are Chinese prose; Chinese
   has no word boundaries, so the `(?<![A-Za-z0-9])` boundary trick from the
   transport-vocabulary gate does not apply and would only add failure modes.
   What matters here is the opposite risk: matching something weaker than the
   spec. See point 2.

2. Backticks are part of the match. README writes `known`, `text` and `diff` in
   backticks. Dropping them from the expected string would still match the same
   README today, but it would also match a future README where the word appears
   in ordinary prose -- the assertion would stop distinguishing "the required
   constraint survived" from "the word known happens to appear somewhere". The
   phrases below were copied out of README.md byte-for-byte, not retyped.

3. Every phrase is checked separately and reported by its own :FAIL: line, so a
   failure names which constraint was lost rather than just "README is wrong".
"""
import argparse
import sys
from pathlib import Path

TARGET = "README.md"

# Copied byte-for-byte from README.md. The backticks are deliberate -- see the
# module docstring, point 2.
REQUIRED = (
    # 0035-R10: moving an entry into `known` needs explicit confirmation.
    "把条目移入 `known` 必须在同一会话里得到闫博明确确认",
    # 0035-R10: the preview (text and diff) must be shown before confirming.
    "把预览的 `text` 和 `diff` 展示给闫博",
    # 0035-R9-AC1: the pre-existing sentence must be preserved unchanged.
    "学习与理解程度仍由项目所有者判断",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    path = args.source / TARGET
    if not path.is_file():
        print(":FAIL: 0035_R9_AC1_readme_constraints: %s is missing" % TARGET,
              flush=True)
        return 1

    text = path.read_text(encoding="utf-8")
    missing = [phrase for phrase in REQUIRED if phrase not in text]

    print("0035-R9-AC1 readme: checked %d required phrases in %s"
          % (len(REQUIRED), TARGET), flush=True)
    if missing:
        for phrase in missing:
            print(":FAIL: 0035_R9_AC1_readme_constraints: missing phrase: %r"
                  % phrase, flush=True)
        return 1
    for phrase in REQUIRED:
        print("  present: %r" % phrase, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
