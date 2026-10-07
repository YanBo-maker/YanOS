#!/usr/bin/env python3
"""0026 acceptance through the production RV32 terminal and real disk images.

Expected rows are hand-listed and use CPython's UTF-8 display oracle. Command
echoes are excluded from each comparison; read-only runs compare the entire
image. Missing repository inputs are hard failures, never dependency skips.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_search as literal
import run_terminal_files as base

INPUTS = tuple(literal.PRODUCTION_INPUTS) + (
    "tests/guest/run_search_terms.py", "os/memory.h",
    "os/search_terms.h", "os/search_terms.c",
    "os/search_terms_core.h", "os/search_terms_core.c",
    "os/search_terms_linear.h", "os/search_terms_linear.c",
    "os/search_terms_index.h", "os/search_terms_index.c",
    "os/search_text.h", "os/search_text.c",
)


def summary(total, shown=None, skipped=0, mode="index"):
    if shown is None:
        shown = min(20, total)
    return ("OK search total=%d shown=%d skipped=%d mode=%s\r\n"
            % (total, shown, skipped, mode)).encode()


def row(name, line, content, left=False, right=False):
    return (name.encode() + b":" + str(line).encode() + b":" +
            (b"..." if left else b"") + base.display(content) +
            (b"..." if right else b"") + b"\r\n")


def status(state, terms=0, postings=0):
    return ("INDEX state=%s source=MOUNTED terms=%d postings=%d\r\nOK index\r\n"
            % (state, terms, postings)).encode()


def run_commands(args, work, name, files, expectations, readonly=True,
                 disk=None):
    if disk is None:
        disk = work / (name + ".img")
        base.seed_image(disk, files)
    before = disk.read_bytes()
    commands = [command for command, _ in expectations]
    result = base.run_guest(args.run, args.guest, disk,
                            b"\n".join(commands + [b"exit"]) + b"\n",
                            work, name)
    literal.require_rc(result, 0, name)
    cursor = 0
    for command, expected in expectations:
        # Find each repeated command after its predecessor, never in an echo
        # or in a previous result. Editor commands are checked separately.
        prefix = b"yanfs> " + command + b"\r\n"
        at = result.stdout.find(prefix, cursor)
        if at < 0:
            raise AssertionError("%s: missing command %r" % (name, command))
        start = at + len(prefix)
        end = result.stdout.find(b"yanfs> ", start)
        if end < 0:
            raise AssertionError("%s: missing next prompt" % name)
        actual = result.stdout[start:end]
        if actual != expected:
            raise AssertionError("%s: %r\nactual %r\nexpected %r"
                                 % (name, command, actual, expected))
        cursor = end
    if readonly:
        literal.unchanged(disk, before, name)
    return disk, result


def case_words_and_rank(args, work):
    files = [("irq.md", b"Interrupt controller handles " +
              "中断".encode() + b"\nPLIC handles interrupt interrupt\n", 1),
             ("trap.md", b"interruption pending\n", 2)]
    first = b"Interrupt controller handles " + "中断".encode()
    second = b"PLIC handles interrupt interrupt"
    run_commands(args, work, "words", files, [
        (b"index status", status("EMPTY")),
        (b"search interrupt", row("irq.md", 2, second) +
         row("irq.md", 1, first) + summary(2)),
        (b"search interrupt PLIC", row("irq.md", 2, second) + summary(1)),
        (b"search CPU", summary(0)),
        (b"grep interrupt", row("irq.md", 2, second) +
         row("trap.md", 1, b"interruption pending") + b"OK grep\r\n"),
        (b"index clear", b"OK index\r\n"),
        (b"index status", status("EMPTY")),
        (b"rebuild", b"OK rebuild\r\n"),
        (b"search INTERRUPT interrupt", row("irq.md", 2, second) +
         row("irq.md", 1, first) + summary(2)),
    ])


def case_unicode_and_invalid(args, work):
    body = ("CPU处理中断\nCPU 中 foo 断\n中，断 café\n".encode() +
            b"hit\r\nhit\rhit\nhit\t\\\x1b")
    files = [("good", body, 1), ("nul", b"hit\n\x00", 2),
             ("bad", b"hit\n\xf0\x9f", 3)]
    run_commands(args, work, "unicode", files, [
        ("search CPU处理中断".encode(), row("good", 1,
         "CPU处理中断".encode()) + summary(1, skipped=2)),
        ("search 中断".encode(), row("good", 1,
         "CPU处理中断".encode()) + summary(1, skipped=2)),
        ("search 中 断".encode(), row("good", 1, "CPU处理中断".encode()) +
         row("good", 2, "CPU 中 foo 断".encode()) +
         row("good", 3, "中，断 café".encode()) + summary(3, skipped=2)),
        ("search 中，断 café".encode(), row("good", 3,
         "中，断 café".encode()) + summary(1, skipped=2)),
        (b"search hit", row("good", 5, b"hit\rhit") +
         row("good", 4, b"hit") + row("good", 6, b"hit\t\\\x1b") +
         summary(3, skipped=2)),
    ])


def case_lifecycle(args, work):
    disk, _ = run_commands(args, work, "lifecycle", [("a", b"hit", 1)], [
        (b"search hit", row("a", 1, b"hit") + summary(1)),
        (b"index status", status("READY", 1, 1)),
        (b"mv a a", b"OK mv\r\n"),
        (b"index status", status("READY", 1, 1)),
        (b"cp a b", b"OK cp\r\n"),
        (b"index status", status("STALE")),
        (b"search hit", row("a", 1, b"hit") + row("b", 1, b"hit") + summary(2)),
        (b"mv a c", b"OK mv\r\n"),
        (b"search hit", row("c", 1, b"hit") + row("b", 1, b"hit") + summary(2)),
        (b"write c newer", b"OK write\r\n"),
        (b"search hit", row("b", 1, b"hit") + summary(1)),
        (b"rm b", b"OK rm\r\n"),
        (b"search hit", summary(0)),
    ], readonly=False)
    # RAM index must disappear in a new process while the changed note remains.
    run_commands(args, work, "restart", [], [
        (b"index status", status("EMPTY")),
        (b"search newer", row("c", 1, b"newer") + summary(1)),
        (b"index status", status("READY", 1, 1)),
    ], disk=disk)


def case_editor_save_cancel(args, work):
    disk = work / "editor.img"
    base.seed_image(disk, [])
    commands = ("edit note\na CPU处理中断\nsearch CPU\nw\n"
                "search CPU 中断\nedit note\nr 1 changed\nq\n"
                "search changed\nexit\n").encode()
    result = base.run_guest(args.run, args.guest, disk, commands, work, "editor")
    literal.require_rc(result, 0, "editor")
    literal.require_fragment(result,
        b"edit> search CPU\r\nERROR UNKNOWN_COMMAND\r\n", "editor-routing")
    literal.expect_interval(result.stdout, "search CPU 中断".encode(),
        row("note", 1, "CPU处理中断".encode()) + summary(1), "editor-save")
    literal.expect_interval(result.stdout, b"search changed", summary(0),
                            "editor-cancel")
    # Validate the published file independently; the cancelled draft is absent.
    entries = base.parse_entries(disk.read_bytes(), base.BLOCKS)
    if len(entries) != 1 or entries[0][1] != "note":
        raise AssertionError("editor: wrong directory")
    _, _, length, lba, _ = entries[0]
    saved = disk.read_bytes()[lba * base.BLOCK:lba * base.BLOCK + length]
    if saved != "CPU处理中断\n".encode():
        raise AssertionError("editor: saved/cancelled content differs")


def case_bounded_snippet(args, work):
    text = "界" * 1500 + " hit " + "🙂" * 300
    expected = "界" * 39 + " hit " + "🙂" * 116
    # Anchor is h: the nearest 40 scalars are 39 '界' and the leading space.
    run_commands(args, work, "snippet", [("long", text.encode(), 1)], [
        (b"search hit", row("long", 1, expected.encode(), True, True) + summary(1)),
        (b"search hit", row("long", 1, expected.encode(), True, True) + summary(1)),
    ])


def case_capacity_fallback(args, work):
    body = b"a" * 256 + b" hit\nhit hit\n"
    run_commands(args, work, "limit", [("f", body, 1)], [
        (b"rebuild", b"ERROR INDEX_LIMIT\r\n"),
        (b"index status", status("LIMIT")),
        (b"search hit", row("f", 2, b"hit hit") +
         row("f", 1, b"a" * 39 + b" hit", True) + summary(2, mode="scan")),
        (b"search hit", row("f", 2, b"hit hit") +
         row("f", 1, b"a" * 39 + b" hit", True) + summary(2, mode="scan")),
        (b"index clear", b"OK index\r\n"),
        (b"index status", status("EMPTY")),
    ])


def case_query_limits(args, work):
    invalid = [b"search", b"search -hit", b'search "hit',
               b'search hit"', b'search ""', b'search "hit" tail',
               b"search !!!", b"index missing", b"rebuild extra",
               b"search " + b" ".join(("g%d" % n).encode() for n in range(17))]
    expected = [(cmd, b"ERROR USAGE\r\n") for cmd in invalid]
    expected += [(b"search " + b" ".join([b"HIT", b"hit"] * 9),
                  row("f", 1, b"hit") + summary(1))]
    # Legal maximal byte length uses one non-ASCII group, not a forbidden
    # 1016-byte ASCII word; quoted/bare limits come from the whole line.
    expected += [(b"search " + "界".encode() * 338 + b"!!", summary(0)),
                 (b'search "' + "界".encode() * 338 + b'"', summary(0))]
    run_commands(args, work, "grammar", [("f", b"hit", 1)], expected)


def case_top20_and_cap(args, work):
    body = (b"hit\n" * 24 + b"hit " * 260 + b"\n")
    expected = row("f", 25, (b"hit " * 40), right=True)
    expected += b"".join(row("f", line, b"hit") for line in range(1, 20))
    run_commands(args, work, "top20", [("f", body, 1)], [
        (b"search hit", expected + summary(25)),
        (b"search HIT hit", expected + summary(25)),
    ])


CASES = [case_words_and_rank, case_unicode_and_invalid, case_lifecycle,
         case_editor_save_cancel, case_bounded_snippet, case_capacity_fallback,
         case_query_limits, case_top20_and_cap]


def main():
    parser = argparse.ArgumentParser()
    for flag in ("source", "work", "run", "mkfs", "guest"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args()
    missing = [args.source / item for item in INPUTS
               if not (args.source / item).is_file()]
    missing += [path for path in (args.run, args.mkfs, args.guest)
                if not path.is_file()]
    if missing:
        for path in missing:
            print(":FAIL: missing required input: %s" % path)
        return 1
    failed = harness = 0
    for case in CASES:
        work = args.work / case.__name__
        work.mkdir(parents=True, exist_ok=True)
        try:
            case(args, work)
            print("PASS " + case.__name__, flush=True)
        except AssertionError as error:
            failed += 1
            print(":FAIL: %s: %s" % (case.__name__, error), flush=True)
        except (base.HarnessError, OSError, subprocess.TimeoutExpired) as error:
            harness += 1
            print(":HARNESS-ERROR: %s: %s" % (case.__name__, error), flush=True)
    print("search_terms Guest: %d passed, %d failed, %d harness errors"
          % (len(CASES) - failed - harness, failed, harness), flush=True)
    return 2 if harness else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
