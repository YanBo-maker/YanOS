#!/usr/bin/env python3
"""Acceptance for the gated 0027:83 metadata listing tool (yanos_note_list).

Every assertion maps to one AC of the listing spec. The two properties worth
calling out:

* "check every file's header first" (0027:83) is asserted directly: a library
  with one broken header must return METADATA and no names, never a prefix of
  the list. The test proves "no partial list" by checking the reply carries no
  ``names`` array at all, not merely that it is empty.
* Tag filters are ANDed, and duplicates are deduplicated BEFORE the 16-slot
  budget is spent, so ``["cpu","cpu"]`` must behave exactly like ``["cpu"]``.

``--case 0041-R2-AC<n>`` runs only the functions that guard that AC, so a
labelled ``ctest -L 0041-R2-AC<n>`` proves that AC instead of merely proving
that the whole script still passes. Without ``--case`` every function runs, so
the aggregate test keeps its original coverage. An unknown case name is a
usage error (exit 2) rather than a silent zero-case pass: a typo must never
look like a green AC.

No existing test expectation is modified here.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from mcp_checks import META, TOOLS_0033_SHA256, Wire, json_member
from semantic_fixture import image

CASES = 0
SIX = ["yanos_search", "yanos_index_status", "yanos_block_list",
       "yanos_block_read", "yanos_block_refs", "yanos_block_backlinks"]
META_TOOLS = ["yanos_note_meta", "yanos_note_list"]
WRITE = ["yanos_note_create", "yanos_note_replace"]
LIST = "yanos_note_list"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def note(tags=(), title="T", body="B\n"):
    """A 0027 note with the given tags, in the order supplied."""
    head = "@yan-note 1\ntitle: %s\n" % title
    for tag in tags:
        head += "tag: %s\n" % tag
    return (head + "@end\n@block log\n" + body).encode("utf-8")


class Server:
    def __init__(self, binary, image_path, extra=()):
        self.image = Path(image_path)
        self.ident = 0
        args = [str(binary), "--disk-image", str(self.image),
                "--index", str(self.image) + ".index",
                "--model", str(self.image) + ".model",
                "--runner", str(self.image) + ".runner", *extra]
        self.wire = Wire(args, dict(os.environ))

    def tools(self):
        self.ident += 1
        self.wire.send("tools/list", ident=self.ident)
        return self.wire.get()["result"]["tools"]

    def call(self, name, arguments):
        self.ident += 1
        self.wire.send("tools/call", ident=self.ident,
                       params={"name": name, "arguments": arguments})
        reply = self.wire.get()
        result = reply["result"]
        value = json.loads(result["content"][0]["text"])
        if result["isError"]:
            return None, value
        return value, None

    def close(self):
        self.wire.close()


def expect_none(value, error, label):
    assert error is None, (label, error)
    return value


def expect_error(error, code, label):
    assert error == {"code": code}, (label, error)


def make_image(path, files, capacity=None):
    Path(path).write_bytes(image(files, capacity))
    return Path(path)


LIBRARY = [
    ("a-cpu.txt", note(["cpu", "riscv"])),
    ("b-cpu.txt", note(["cpu"])),
    ("c-os.txt", note(["os"])),
    ("d-none.txt", note([])),
    ("e-old.txt", b"no header here\n"),          # header-less old note
]

# Which functions guard which 0041-R2 AC. A function may guard more than one
# AC only when it genuinely asserts both (for example the flag-gating function
# checks the shipped tool surface AND that the refused call does not write).
# Every AC in AC_CASES must have at least one owner, and every owner must exist
# as a function, so a renamed or dropped function cannot silently drop an AC.
AC_CASES = {
    "0041-R2-AC1": ["test_default_start_has_no_meta_tools"],
    "0041-R2-AC2": ["test_list_all_without_filter",
                    "test_headerless_old_note_is_listed"],
    "0041-R2-AC3": ["test_single_tag_filter"],
    "0041-R2-AC4": ["test_two_tags_are_anded"],
    "0041-R2-AC5": ["test_empty_result_still_succeeds"],
    "0041-R2-AC6": ["test_broken_header_is_metadata_without_partial_list",
                    "test_broken_header_with_filter_also_metadata"],
    "0041-R2-AC7": ["test_duplicate_tags_are_deduplicated"],
    "0041-R2-AC8": ["test_more_than_sixteen_distinct_tags_is_invalid",
                    "test_bad_tag_bytes_are_invalid"],
    "0041-R2-AC9": ["test_stale_precedes_whole_library_scan",
                    "test_fresh_identity_succeeds"],
    "0041-R2-AC10": ["test_default_start_has_no_meta_tools"],
    "0041-R2-AC11": ["test_list_is_read_only"],
}

# Tool-surface functions. AC1 and AC10 own the default-start function; the
# other three surfaces are asserted by the corresponding 0041-R1 ACs in
# mcp_meta_checks.py, so they stay in the aggregate run only.
# Minimum number of assert statements each AC must actually EXECUTE. A per-AC
# CTest entry proves the AC ran its own functions, not that those functions
# still assert anything: deleting an assertion (or gutting a function body)
# leaves a green run as long as nothing raises. Measured with sys.settrace on
# the real binary; a lowered floor is a deliberate, reviewable edit, while an
# accidental deletion fails the case.
MIN_ASSERTS = {
    "0041-R2-AC1": 5,
    "0041-R2-AC2": 17,
    "0041-R2-AC3": 3,
    "0041-R2-AC4": 4,
    "0041-R2-AC5": 4,
    "0041-R2-AC6": 5,
    "0041-R2-AC7": 7,
    "0041-R2-AC8": 6,
    "0041-R2-AC9": 7,
    "0041-R2-AC10": 5,
    "0041-R2-AC11": 1,
}

_THIS_FILE = __file__
_ASSERT_LINES = {
    number + 1
    for number, text in enumerate(open(__file__, encoding="utf-8").read().splitlines())
    if text.strip().startswith("assert ")
}
_EXECUTED = [0]


def _count_asserts(frame, event, arg):
    if event == "call":
        return _count_asserts
    if event == "line" and frame.f_code.co_filename == _THIS_FILE:
        if frame.f_lineno in _ASSERT_LINES:
            _EXECUTED[0] += 1
    return _count_asserts


SURFACE_ONLY = ["test_meta_flag_yields_eight_tools",
                "test_both_flags_yields_ten_tools",
                "test_write_only_flag_is_unchanged"]

ORDER = [
    "test_list_all_without_filter",
    "test_single_tag_filter",
    "test_two_tags_are_anded",
    "test_empty_result_still_succeeds",
    "test_broken_header_is_metadata_without_partial_list",
    "test_broken_header_with_filter_also_metadata",
    "test_duplicate_tags_are_deduplicated",
    "test_more_than_sixteen_distinct_tags_is_invalid",
    "test_bad_tag_bytes_are_invalid",
    "test_headerless_old_note_is_listed",
    "test_stale_precedes_whole_library_scan",
    "test_fresh_identity_succeeds",
    "test_default_start_has_no_meta_tools",
    "test_meta_flag_yields_eight_tools",
    "test_both_flags_yields_ten_tools",
    "test_write_only_flag_is_unchanged",
    "test_list_is_read_only",
]


def library(root, name="lib.img"):
    return make_image(root / name, LIBRARY)


# ---------------------------------------------------------------- 1..4 filter

def test_list_all_without_filter(binary, root):
    """No tags: every file is listed, ASCII name ascending, shown matches."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(LIST, {}), label="all")
        assert value["operation"] == "note_list", value["operation"]
        assert value["names"] == [n for n, _ in LIBRARY], \
            ("all_names", value["names"])
        assert value["shown"] == len(LIBRARY), ("all_shown", value["shown"])
        assert value["shown"] == len(value["names"]), "all_shown_matches_len"
        assert value["names"] == sorted(value["names"]), "all_ascii_ascending"
        assert value["complete"] is True
        CASES += 1
    finally:
        server.close()


def test_single_tag_filter(binary, root):
    """One tag: only the files carrying it are listed."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(LIST, {"tags": ["cpu"]}), label="one")
        assert value["names"] == ["a-cpu.txt", "b-cpu.txt"], \
            ("one_names", value["names"])
        assert value["shown"] == 2
        CASES += 1
    finally:
        server.close()


def test_two_tags_are_anded(binary, root):
    """Several tags are ANDed; filter order does not change the result set."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(LIST, {"tags": ["cpu", "riscv"]}),
                            label="and")
        assert value["names"] == ["a-cpu.txt"], ("and_names", value["names"])
        # Reversing the filter order must not change the result set.
        reverse = expect_none(*server.call(LIST, {"tags": ["riscv", "cpu"]}),
                              label="and_rev")
        assert reverse["names"] == value["names"], "and_order_independent"
        CASES += 1
    finally:
        server.close()


def test_empty_result_still_succeeds(binary, root):
    """A filter matching nothing is success with an empty list and shown 0."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(LIST, {"tags": ["nosuchtag"]}),
                            label="empty")
        assert value["names"] == [], ("empty_names", value["names"])
        assert value["shown"] == 0, ("empty_shown", value["shown"])
        assert value["complete"] is True
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------- 5 broken header

def test_broken_header_is_metadata_without_partial_list(binary, root):
    """One broken header anywhere is METADATA, never a partial list, and the
    filesystem stays usable afterwards."""
    global CASES
    files = list(LIBRARY) + [("f-broken.txt", b"@yan-note\ntitle: T\n@end\n")]
    img = make_image(root / "broken.img", files)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value, error = server.call(LIST, {})
        expect_error(error, "METADATA", "broken")
        # "never a partial list" means the failure carries no names array at
        # all -- not an empty one that a caller could mistake for a result.
        assert value is None, ("broken_no_value", value)
        # And the same instance still serves afterwards.
        listed = expect_none(*server.call("yanos_block_list",
                                          {"name": "a-cpu.txt"}),
                             label="broken_after")
        assert isinstance(listed.get("blocks"), list), "broken_fs_still_usable"
        CASES += 1
    finally:
        server.close()


def test_broken_header_with_filter_also_metadata(binary, root):
    """The broken-header failure is library-wide, not filter-dependent."""
    global CASES
    files = list(LIBRARY) + [("f-broken.txt", b"@yan-note\ntitle: T\n@end\n")]
    img = make_image(root / "broken2.img", files)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        _, error = server.call(LIST, {"tags": ["cpu"]})
        expect_error(error, "METADATA", "broken_filtered")
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------- 6..8 tag arguments

def test_duplicate_tags_are_deduplicated(binary, root):
    """Repeated tags behave like a single one and do not consume 16 slots."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        once = expect_none(*server.call(LIST, {"tags": ["cpu"]}), label="dedup1")
        twice = expect_none(*server.call(LIST, {"tags": ["cpu", "cpu"]}),
                            label="dedup2")
        tripled = expect_none(*server.call(LIST,
                                           {"tags": ["cpu", "cpu", "cpu"]}),
                              label="dedup3")
        assert twice["names"] == once["names"], ("dedup_two", twice["names"])
        assert tripled["names"] == once["names"], "dedup_three"
        # Dedup happens before the 16-slot budget is spent: 16 copies of one
        # tag plus a second distinct tag must still be accepted.
        many = ["cpu"] * 16 + ["riscv"]
        accepted = expect_none(*server.call(LIST, {"tags": many}),
                               label="dedup_budget")
        assert accepted["names"] == ["a-cpu.txt"], \
            ("dedup_budget_names", accepted["names"])
        CASES += 1
    finally:
        server.close()


def test_more_than_sixteen_distinct_tags_is_invalid(binary, root):
    """Seventeen distinct tags exceed the budget; sixteen are still legal."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        seventeen = ["t%02d" % i for i in range(17)]
        _, error = server.call(LIST, {"tags": seventeen})
        expect_error(error, "INVALID", "too_many_tags")
        # Exactly 16 distinct is still legal.
        sixteen = ["t%02d" % i for i in range(16)]
        value = expect_none(*server.call(LIST, {"tags": sixteen}), label="16ok")
        assert value["names"] == [], "sixteen_distinct_accepted"
        CASES += 1
    finally:
        server.close()


def test_bad_tag_bytes_are_invalid(binary, root):
    """An empty tag, an over-long tag or a tag with a space is INVALID."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        _, error = server.call(LIST, {"tags": [""]})
        expect_error(error, "INVALID", "empty_tag")
        _, error = server.call(LIST, {"tags": ["x" * 64]})
        expect_error(error, "INVALID", "long_tag")
        # A tag with a space is not a legal 0027 tag either.
        _, error = server.call(LIST, {"tags": ["two words"]})
        expect_error(error, "INVALID", "space_tag")
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------- 9 header-less

def test_headerless_old_note_is_listed(binary, root):
    """A header-less old note is listed; it simply carries no tags."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(LIST, {}), label="old_listed")
        assert "e-old.txt" in value["names"], ("old_present", value["names"])
        # It carries no tags, so a tag filter excludes it.
        filtered = expect_none(*server.call(LIST, {"tags": ["cpu"]}),
                               label="old_filtered")
        assert "e-old.txt" not in filtered["names"], "old_not_tagged"
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------- 10 stale ordering

def test_stale_precedes_whole_library_scan(binary, root):
    """A stale identity beats a METADATA the whole-library scan would find."""
    global CASES
    files = list(LIBRARY) + [("f-broken.txt", b"@yan-note\ntitle: T\n@end\n")]
    img = make_image(root / "stale.img", files)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        stale = "0" * 64
        # The library contains a broken header, so a scan would be METADATA;
        # a stale identity must win because it is decided first.
        _, error = server.call(LIST, {"expected_source": stale})
        expect_error(error, "STALE", "stale_before_metadata")
        # Same for a clean library, with and without a filter.
        clean = library(root, "stale2.img")
        server2 = Server(binary, clean, extra=("--allow-meta",))
        try:
            _, error = server2.call(LIST, {"expected_source": stale})
            expect_error(error, "STALE", "stale_clean")
            _, error = server2.call(LIST, {"tags": ["cpu"],
                                           "expected_source": stale})
            expect_error(error, "STALE", "stale_filtered")
        finally:
            server2.close()
        CASES += 1
    finally:
        server.close()


def test_fresh_identity_succeeds(binary, root):
    """The current identity is accepted, with unchanged names and identity."""
    global CASES
    img = library(root)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        first = expect_none(*server.call(LIST, {}), label="fresh1")
        current = first["source_sha256"]
        again = expect_none(*server.call(LIST, {"expected_source": current}),
                            label="fresh2")
        assert again["names"] == first["names"], "fresh_same_names"
        assert again["source_sha256"] == current, "fresh_same_identity"
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------- 11..12 gating

def test_default_start_has_no_meta_tools(binary, root):
    """No flag: the listing tool is absent from tools/list, calling it behaves
    exactly like an unknown name, and the image is untouched."""
    global CASES
    img = library(root, "gate.img")
    before = digest(img)
    server = Server(binary, img, extra=())
    try:
        server.ident += 1
        server.wire.send("tools/list", ident=server.ident)
        raw = server.wire.read_line()
        tools = json.loads(raw)["result"]["tools"]
        names = [t["name"] for t in tools]
        assert names == SIX, ("default_names", names)
        assert hashlib.sha256(json_member(raw, "tools")).hexdigest() == \
            TOOLS_0033_SHA256, "default_pinned_baseline_changed"
        # And the listing tool is not callable in this state.
        server.ident += 1
        server.wire.send("tools/call", ident=server.ident,
                         params={"name": LIST, "arguments": {}})
        reply = server.wire.get()
        assert reply["error"]["code"] == -32602, ("default_call", reply)
        # R2-AC10 second half: a refused call must not write the image.
        assert digest(img) == before, "default_call_wrote_image"
        CASES += 1
    finally:
        server.close()


def test_meta_flag_yields_eight_tools(binary, root):
    """--allow-meta adds both metadata tools after the six read-only ones."""
    global CASES
    img = library(root, "gate2.img")
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        names = [t["name"] for t in server.tools()]
        assert names == SIX + META_TOOLS, ("meta8", names)
        assert len(names) == 8, ("meta8_count", len(names))
        CASES += 1
    finally:
        server.close()


def test_both_flags_yields_ten_tools(binary, root):
    """Both flags yield the documented ten-tool order."""
    global CASES
    img = library(root, "gate3.img")
    server = Server(binary, img, extra=("--allow-meta", "--allow-write"))
    try:
        names = [t["name"] for t in server.tools()]
        assert names == SIX + META_TOOLS + WRITE, ("both10", names)
        assert len(names) == 10, ("both10_count", len(names))
        CASES += 1
    finally:
        server.close()


def test_write_only_flag_is_unchanged(binary, root):
    """--allow-write alone is untouched by the metadata surface."""
    global CASES
    img = library(root, "gate4.img")
    server = Server(binary, img, extra=("--allow-write",))
    try:
        names = [t["name"] for t in server.tools()]
        assert names == SIX + WRITE, ("write8", names)
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------- zero writes

def test_list_is_read_only(binary, root):
    """Every call, successful or not, leaves the image byte-identical."""
    global CASES
    files = list(LIBRARY) + [("f-broken.txt", b"@yan-note\ntitle: T\n@end\n")]
    img = make_image(root / "ro.img", files)
    before = digest(img)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        server.call(LIST, {})                                   # METADATA
        server.call(LIST, {"tags": ["cpu"]})                    # METADATA
        server.call(LIST, {"expected_source": "0" * 64})        # STALE
        assert digest(img) == before, "list_wrote_image"
        CASES += 1
    finally:
        server.close()


def main():
    argv = sys.argv[1:]
    case = None
    if "--case" in argv:
        index = argv.index("--case")
        if index + 1 >= len(argv):
            print("FAIL --case needs a value", file=sys.stderr)
            return 2
        case = argv[index + 1]
        del argv[index:index + 2]
    if len(argv) != 1:
        print("usage: mcp_note_list_checks.py YAN_MCP [--case 0041-R2-AC<n>]",
              file=sys.stderr)
        return 2
    binary = Path(argv[0]).resolve()
    if not binary.is_file():
        print("FAIL fixture missing: %s" % binary, file=sys.stderr)
        return 1

    # The AC table must stay consistent with the functions that exist, so a
    # renamed function or a new AC cannot silently shrink a labelled run.
    declared = set()
    for names in AC_CASES.values():
        declared.update(names)
    known = declared | set(SURFACE_ONLY)
    missing = sorted(known - set(ORDER))
    unowned = sorted(set(ORDER) - known)
    if missing:
        print("FAIL AC_CASES names unknown functions: %s" % missing,
              file=sys.stderr)
        return 1
    if unowned:
        print("FAIL functions not owned by any case: %s" % unowned,
              file=sys.stderr)
        return 1

    if case is None:
        selected = list(ORDER)
    else:
        if case not in AC_CASES:
            # A typo must fail loudly: a zero-case run that exits 0 would let a
            # mislabelled AC look green.
            print("FAIL unknown case %r; known: %s"
                  % (case, ", ".join(sorted(AC_CASES))), file=sys.stderr)
            return 2
        selected = [n for n in ORDER if n in AC_CASES[case]]
        if not selected:
            print("FAIL case %r selects no function" % case, file=sys.stderr)
            return 1

    with tempfile.TemporaryDirectory(prefix="yan-note-list-") as directory:
        root = Path(directory)
        _EXECUTED[0] = 0
        sys.settrace(_count_asserts)
        try:
            for name in selected:
                globals()[name](binary, root)
        finally:
            sys.settrace(None)
    executed = _EXECUTED[0]
    if case is not None:
        floor = MIN_ASSERTS[case]
        if executed < floor:
            print("FAIL %s executed %d assertions, expected at least %d"
                  % (case, executed, floor), file=sys.stderr)
            return 1
    print("note_list acceptance: %d cases passed, %d assertions%s"
          % (CASES, executed, "" if case is None else " (%s)" % case))
    return 0


if __name__ == "__main__":
    sys.exit(main())
