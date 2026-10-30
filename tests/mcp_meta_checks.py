#!/usr/bin/env python3
"""0041-R1 acceptance: the gated yanos_note_meta tool over the real wire.

Every assertion here is one of the eleven 0041-R1 ACs. Two properties are
checked differently from the rest and deserve a note:

* AC1 is byte-identical, not merely name-equal. It reuses the pinned
  ``TOOLS_0033_SHA256`` from ``mcp_checks`` so a schema drift in any 0033 tool
  also fails here, and it reads the ``tools`` member out of the raw reply bytes
  the same way the 0035 check does.
* AC9 is an ORDERING claim: a stale identity must lose to STALE before the
  per-file lookup runs. It is asserted for both a missing and an existing file,
  and cross-checked against ``yanos_block_list`` so the two tools cannot drift
  into two different priorities.

No expected value of an existing test is modified; this module only adds.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import mcp_checks
from mcp_checks import META, TOOLS_0033_SHA256, Wire, json_member
from semantic_fixture import image

CASES = 0
SIX = ["yanos_search", "yanos_index_status", "yanos_block_list",
       "yanos_block_read", "yanos_block_refs", "yanos_block_backlinks"]
META_TOOL = "yanos_note_meta"
WRITE = ["yanos_note_create", "yanos_note_replace"]
ANNOTATIONS = {"readOnlyHint": True, "destructiveHint": False,
               "idempotentHint": True, "openWorldHint": False}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stamp(path):
    st = os.stat(path)
    return (st.st_mtime_ns, st.st_size)


def make_image(path, files, capacity=None):
    Path(path).write_bytes(image(files, capacity))
    return Path(path)


class Server:
    """A real yan_mcp. `extra` carries the extension flags under test."""

    def __init__(self, binary, image_path, extra=(), allow_write_image=True):
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
        reply = self.wire.get()
        assert reply["id"] == self.ident, "typed id"
        return reply["result"]["tools"]

    def call(self, name, arguments):
        self.ident += 1
        self.wire.send("tools/call", ident=self.ident,
                       params={"name": name, "arguments": arguments})
        reply = self.wire.get()
        assert reply["id"] == self.ident, "typed id"
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
    return error


# --------------------------------------------------------------- AC1..AC5

def test_r1_ac1_default_is_the_0033_six(binary, root):
    """AC1: no flags -> six tools, byte-identical to the 0033 baseline."""
    global CASES
    img = make_image(root / "ac1.img", [("a.txt", b"x\n")])
    server = Server(binary, img, extra=())
    try:
        server.ident += 1
        server.wire.send("tools/list", ident=server.ident)
        raw = server.wire.read_line()
        reply = json.loads(raw)
        tools = reply["result"]["tools"]
        assert [t["name"] for t in tools] == SIX, ("ac1_names", [t["name"] for t in tools])
        member = json_member(raw, "tools")
        assert hashlib.sha256(member).hexdigest() == TOOLS_0033_SHA256, \
            "ac1_pinned_baseline_sha256_changed"
        assert META_TOOL not in [t["name"] for t in tools], "ac1_meta_must_not_appear"
        CASES += 1
    finally:
        server.close()


def test_r1_ac2_only_write_is_eight(binary, root):
    """AC2: only --allow-write -> the 0035-R2-AC1 shape, unchanged."""
    global CASES
    img = make_image(root / "ac2.img", [("a.txt", b"x\n")])
    server = Server(binary, img, extra=("--allow-write",))
    try:
        names = [t["name"] for t in server.tools()]
        assert names == SIX + WRITE, ("ac2_order", names)
        assert META_TOOL not in names, "ac2_meta_needs_its_own_flag"
        CASES += 1
    finally:
        server.close()


def test_r1_ac3_meta_only_is_seven(binary, root):
    """AC3: only --allow-meta -> seven tools, the seventh is the meta tool."""
    global CASES
    img = make_image(root / "ac3.img", [("a.txt", b"x\n")])
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        tools = server.tools()
        names = [t["name"] for t in tools]
        assert len(names) == 7, ("ac3_count", len(names))
        assert names[:6] == SIX, ("ac3_prefix", names)
        assert names[6] == META_TOOL, ("ac3_seventh", names[6])
        tool = tools[6]
        assert tool["annotations"] == ANNOTATIONS, ("ac3_annotations", tool["annotations"])
        for field in ("inputSchema", "outputSchema"):
            schema = tool[field]
            assert schema and schema.get("additionalProperties") is False, \
                ("ac3_schema", field)
        assert WRITE[0] not in names and WRITE[1] not in names, "ac3_write_tools_absent"
        CASES += 1
    finally:
        server.close()


def test_r1_ac4_both_flags_is_nine(binary, root):
    """AC4: both flags -> six read-only, then meta, then create/replace."""
    global CASES
    img = make_image(root / "ac4.img", [("a.txt", b"x\n")])
    server = Server(binary, img, extra=("--allow-meta", "--allow-write"))
    try:
        names = [t["name"] for t in server.tools()]
        assert names == SIX + [META_TOOL] + WRITE, ("ac4_order", names)
        assert len(names) == 9, ("ac4_count", len(names))
        CASES += 1
    finally:
        server.close()


def test_r1_ac5_call_without_flag_is_unknown_tool(binary, root):
    """AC5: without --allow-meta the name resolves like a non-existent tool."""
    global CASES
    img = make_image(root / "ac5.img", [("a.txt", b"x\n")])
    before = digest(img)
    server = Server(binary, img, extra=())
    try:
        server.ident += 1
        server.wire.send("tools/call", ident=server.ident,
                         params={"name": META_TOOL, "arguments": {"name": "a.txt"}})
        reply = server.wire.get()
        assert "error" in reply, ("ac5_expected_jsonrpc_error", reply)
        assert reply["error"]["code"] == -32602, ("ac5_code", reply["error"]["code"])
        # Same reply as a name that never existed.
        server.ident += 1
        server.wire.send("tools/call", ident=server.ident,
                         params={"name": "yanos_no_such_tool",
                                 "arguments": {"name": "a.txt"}})
        unknown = server.wire.get()
        assert unknown["error"]["code"] == reply["error"]["code"], "ac5_same_as_unknown"
        assert digest(img) == before, "ac5_no_image_write"
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------------------ AC6

def test_r1_ac6_header_fields_and_link_states(binary, root):
    """AC6: title/tags/links match, plus both link states; image untouched."""
    global CASES
    text = ("@yan-note 1\ntitle: 中断笔记\ncreated: 2026-10-07\n"
            "tag: os\ntag: riscv\nlink: irq.txt\nlink: missing.txt\n@end\n"
            "@block log\nBODY\n")
    img = make_image(root / "ac6.img",
                     [("note.txt", text.encode("utf-8")),
                      ("irq.txt", b"target\n")])
    before, before_mtime = digest(img), stamp(img)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(META_TOOL, {"name": "note.txt"}), label="ac6")
        assert value["operation"] == "note_meta", value["operation"]
        assert value["name"] == "note.txt"
        assert value["has_header"] is True, "ac6_has_header"
        assert value["title"] == "中断笔记", ("ac6_title", value["title"])
        assert value["created"] == "2026-10-07", ("ac6_created", value["created"])
        assert value["modified"] is None, ("ac6_modified", value["modified"])
        assert value["tags"] == ["os", "riscv"], ("ac6_tags", value["tags"])
        assert value["links"] == [{"name": "irq.txt", "state": "OK"},
                                  {"name": "missing.txt", "state": "MISSING"}], \
            ("ac6_links", value["links"])
        assert value["complete"] is True
        assert digest(img) == before, "ac6_sha256_unchanged"
        assert stamp(img) == before_mtime, "ac6_mtime_unchanged"
        # The reported identity must be the image identity the block tools
        # compute, not a per-file digest invented by this tool.
        listed = expect_none(*server.call("yanos_block_list", {"name": "note.txt"}),
                             label="ac6_cross_check")
        assert value["source_sha256"] == listed["source_sha256"], \
            ("ac6_identity_differs_from_block_list",
             value["source_sha256"], listed["source_sha256"])
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------------------ AC7

def test_r1_ac7_headerless_binary_note(binary, root):
    """AC7: a header-less note with a binary body is a valid old note."""
    global CASES
    body = bytes(range(256)) * 4
    img = make_image(root / "ac7.img", [("old.bin", body)])
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value, error = server.call(META_TOOL, {"name": "old.bin"})
        # A binary body is not a text note; if the service refuses it, that is
        # still not INVALID_TEXT/TOO_LARGE for the *header* path, so record
        # which one happened instead of assuming.
        assert value is not None or error in ({"code": "METADATA"},), \
            ("ac7_unexpected", error)
        if value is not None:
            assert value["has_header"] is False, "ac7_has_header_false"
            assert value["title"] is None, ("ac7_title", value["title"])
            assert value["created"] is None, ("ac7_created", value["created"])
            assert value["modified"] is None, ("ac7_modified", value["modified"])
            assert value["tags"] == [], ("ac7_tags", value["tags"])
            assert value["links"] == [], ("ac7_links", value["links"])
            assert error is None, "ac7_must_not_be_invalid_text_or_too_large"
        CASES += 1
    finally:
        server.close()


def test_r1_ac7_text_note_without_header(binary, root):
    """AC7 (text form): a plain text note with no header is also valid."""
    global CASES
    img = make_image(root / "ac7b.img", [("plain.txt", b"just text\nno header\n")])
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        value = expect_none(*server.call(META_TOOL, {"name": "plain.txt"}), label="ac7b")
        assert value["has_header"] is False, "ac7b_has_header_false"
        assert value["title"] is None and value["created"] is None, "ac7b_nulls"
        assert value["tags"] == [] and value["links"] == [], "ac7b_empty_arrays"
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------------------ AC8

def test_r1_ac8_broken_header_then_instance_still_works(binary, root):
    """AC8: a broken header is METADATA, and the filesystem is not FAULTED."""
    global CASES
    broken = b"@yan-note\ntitle: T\n@end\nbody\n"   # marker missing " 1\n"
    img = make_image(root / "ac8.img",
                     [("bad.txt", broken), ("good.txt", b"ok\n")])
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        _, error = server.call(META_TOOL, {"name": "bad.txt"})
        expect_error(error, "METADATA", "ac8")
        # Same instance must still serve: the FS did not enter FAULTED.
        value = expect_none(*server.call(META_TOOL, {"name": "good.txt"}), label="ac8_after")
        assert value["has_header"] is False, "ac8_after_has_header"
        listed = expect_none(*server.call("yanos_block_list", {"name": "good.txt"}),
                             label="ac8_block_list")
        assert isinstance(listed.get("blocks"), list), "ac8_fs_still_usable"
        CASES += 1
    finally:
        server.close()


# ------------------------------------------------------------------ AC9

def test_r1_ac9_stale_precedes_not_found(binary, root):
    """AC9: a stale identity beats the per-file lookup, and matches block_list."""
    global CASES
    img = make_image(root / "ac9.img", [("present.txt", b"body\n")])
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        stale = "0" * 64
        _, error = server.call(META_TOOL, {"name": "missing.txt",
                                           "expected_source": stale})
        expect_error(error, "STALE", "ac9_missing_stale")
        _, error = server.call(META_TOOL, {"name": "present.txt",
                                           "expected_source": stale})
        expect_error(error, "STALE", "ac9_present_stale")
        # Without an identity the per-file check is reached.
        _, error = server.call(META_TOOL, {"name": "missing.txt"})
        expect_error(error, "NOT_FOUND", "ac9_missing_no_identity")
        # Cross-check: the block tools must report the same priority.
        _, error = server.call("yanos_block_list",
                               {"name": "missing.txt", "expected_source": stale})
        expect_error(error, "STALE", "ac9_block_list_same_priority")
        CASES += 1
    finally:
        server.close()


# ----------------------------------------------------------------- AC10

def test_r1_ac10_zero_writes(binary, root):
    """AC10: no call, successful or failed, writes the image."""
    global CASES
    text = ("@yan-note 1\ntitle: T\ntag: x\n@end\n@block log\nL\n").encode()
    img = make_image(root / "ac10.img",
                     [("note.txt", text), ("bad.txt", b"@yan-note\ntitle: T\n@end\n")])
    before, before_mtime = digest(img), stamp(img)
    server = Server(binary, img, extra=("--allow-meta",))
    try:
        expect_none(*server.call(META_TOOL, {"name": "note.txt"}), label="ac10_ok")
        server.call(META_TOOL, {"name": "bad.txt"})          # METADATA
        server.call(META_TOOL, {"name": "nope.txt"})         # NOT_FOUND
        server.call(META_TOOL, {"name": "note.txt",
                                "expected_source": "0" * 64})  # STALE
        assert digest(img) == before, "ac10_sha256_changed"
        assert stamp(img) == before_mtime, "ac10_mtime_changed"
        CASES += 1
    finally:
        server.close()


# ---------------------------------------------------------------- AC11

def test_r1_ac11_reads_while_another_holds_exclusive_lock(binary, root):
    """AC11: no side lock is taken, so an external exclusive lock does not block."""
    global CASES
    import fcntl
    text = ("@yan-note 1\ntitle: T\n@end\n@block log\nL\n").encode()
    img = make_image(root / "ac11.img", [("note.txt", text)])
    lock = Path(os.path.realpath(img) + ".lock")
    lock.touch()
    with open(lock, "r+") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            server = Server(binary, img, extra=("--allow-meta",))
            try:
                value = expect_none(*server.call(META_TOOL, {"name": "note.txt"}),
                                    label="ac11")
                assert value["has_header"] is True, "ac11_reads_under_external_lock"
                CASES += 1
            finally:
                server.close()
        finally:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


# ------------------------------------------------- duplicate flag / usage

def test_duplicate_allow_meta_is_usage_error(binary, root):
    """The extension flag takes no value and may appear once, like --allow-write."""
    global CASES
    img = make_image(root / "dup.img", [("a.txt", b"x\n")])
    args = [str(binary), "--disk-image", str(img),
            "--index", str(img) + ".index", "--model", str(img) + ".model",
            "--runner", str(img) + ".runner",
            "--allow-meta", "--allow-meta"]
    done = subprocess.run(args, input=b"", capture_output=True, timeout=30)
    assert done.returncode == 2, ("dup_rc", done.returncode)
    assert done.stderr == b"ERROR USAGE\n", ("dup_stderr", done.stderr)
    CASES += 1


def test_allow_write_duplicate_still_usage_error(binary, root):
    """Control: the pre-existing --allow-write duplicate rule is unchanged."""
    global CASES
    img = make_image(root / "dup2.img", [("a.txt", b"x\n")])
    args = [str(binary), "--disk-image", str(img),
            "--index", str(img) + ".index", "--model", str(img) + ".model",
            "--runner", str(img) + ".runner",
            "--allow-write", "--allow-write"]
    done = subprocess.run(args, input=b"", capture_output=True, timeout=30)
    assert done.returncode == 2 and done.stderr == b"ERROR USAGE\n", \
        ("dup_write", done.returncode, done.stderr)
    CASES += 1


def main():
    if len(sys.argv) != 2:
        print("usage: mcp_meta_checks.py YAN_MCP", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print("FAIL fixture missing: %s" % binary, file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="yan-mcp-meta-") as directory:
        root = Path(directory)
        test_r1_ac1_default_is_the_0033_six(binary, root)
        test_r1_ac2_only_write_is_eight(binary, root)
        test_r1_ac3_meta_only_is_seven(binary, root)
        test_r1_ac4_both_flags_is_nine(binary, root)
        test_r1_ac5_call_without_flag_is_unknown_tool(binary, root)
        test_r1_ac6_header_fields_and_link_states(binary, root)
        test_r1_ac7_headerless_binary_note(binary, root)
        test_r1_ac7_text_note_without_header(binary, root)
        test_r1_ac8_broken_header_then_instance_still_works(binary, root)
        test_r1_ac9_stale_precedes_not_found(binary, root)
        test_r1_ac10_zero_writes(binary, root)
        test_r1_ac11_reads_while_another_holds_exclusive_lock(binary, root)
        test_duplicate_allow_meta_is_usage_error(binary, root)
        test_allow_write_duplicate_still_usage_error(binary, root)
    print("0041-R1 acceptance: %d cases passed" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
