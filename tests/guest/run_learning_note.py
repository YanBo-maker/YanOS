#!/usr/bin/env python3
"""0035-R8-AC1/AC2 learning-note acceptance over real MCP and Guest binaries.

The checked-in fixtures are the expected bytes.  Identity, block positions,
references and diff lines come from the independent Python oracles in this
repository, never from the server response.  Missing local inputs are hard
failures (exit 1), never a dependency skip.  Assertions fail with ``:FAIL:``
(exit 1); crashes, timeouts and transport errors are ``:HARNESS-ERROR:``
(exit 2).  This script never builds anything and never returns 77.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve()
TESTS = HERE.parents[1]
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(HERE.parent))

import block_references_reference as reference
from note_metadata_reference import parse_header
import run_search_terms
import run_terminal_files as base
from semantic_fixture import image

BLOCK = 4096
MAGIC = b"YANFS01\x00"
LEARNING = "learning-note.md"
ORDINARY = ("irq", "plic", "trap")
FIXTURES = (
    "tests/learning-note/learning-note.md",
    "tests/learning-note/learning-note-updated.md",
    "tests/learning-note/notes/irq",
    "tests/learning-note/notes/plic",
    "tests/learning-note/notes/trap",
)
INPUTS = FIXTURES + (
    "tests/guest/run_learning_note.py",
    "tests/block_references_reference.py",
    "tests/note_metadata_reference.py",
    "tests/guest/run_search_terms.py",
    "tests/guest/run_search.py",
    "tests/guest/run_editor.py",
    "tests/guest/run_terminal_files.py",
    "tests/semantic_fixture.py",
    "tests/semantic_json_checks.py",
    "tests/mcp_note_checks.py",
    "tests/mcp_checks.py",
)

LEARNING_BLOCKS = ("criteria", "known", "unknown", "untouched", "log")
MOVED_LINE = "CLINT 的 mtimecmp 在计时达到比较值时触发中断。\n".encode("utf-8")
PLIC_LINE = "PLIC 的 claim 与 completion 为什么要成对。\n".encode("utf-8")
LOG_ENTRY = (
    "2026-10-21 测试模拟闫博确认：把一条 unknown 移入 known；"
    "这是夹具模拟，不代表真实理解。\n"
    "- 题目：mtimecmp 在什么时候触发中断。\n"
    "- 回答要点：模拟答案，仅用于夹具。\n"
    "- 被指出的问题：模拟记录。\n"
    "- 原文：[[irq#entry]]、[[plic#claim]]\n"
).encode("utf-8")

LIST_KEYS = {"schema_version", "operation", "source_sha256", "name",
             "total", "shown", "blocks"}
BLOCK_KEYS = {"id", "marker_line", "first_line", "last_line", "start", "end"}
READ_KEYS = {"schema_version", "operation", "source_sha256", "name", "block_id",
             "block", "page", "text", "complete", "next_offset"}
REFS_KEYS = {"schema_version", "operation", "source_sha256", "name",
             "total", "shown", "refs"}
REF_ROW_KEYS = {"source", "block_id", "line", "start", "end", "target", "state"}
PREVIEW_KEYS = {"schema_version", "operation", "name", "source_sha256",
                "text", "diff"}
COMMIT_KEYS = {"schema_version", "operation", "name", "bytes",
               "source_sha256", "previous_source_sha256"}


# --------------------------------------------------------------- basic helpers

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def mtime_ns(path):
    return os.stat(path).st_mtime_ns


def logical_hash(files):
    """The 0033 whole-library identity, imported lazily.

    ``semantic_json_checks`` imports ``fcntl`` at module level (Unix-only), so
    the Windows-importable gate must not pull it in from this module.
    """
    from semantic_json_checks import logical_hash as implementation
    return implementation(files)


def _mcp_server(args, image_path, allow_write):
    """Real stdio MCP server, imported lazily for the same portability reason."""
    from mcp_note_checks import Server
    return Server(args.server, image_path, allow_write=allow_write)


def assert_exact(actual, expected, label="value"):
    """Small comparison helper shared with the pure negative-control gate."""
    if actual != expected:
        raise AssertionError("%s differs: actual %r expected %r"
                             % (label, actual, expected))
    return actual


def split_lines(data):
    """The spec's line rule: LF stays with its line; a final non-LF tail is a
    line; empty bytes are zero lines."""
    lines = []
    start = 0
    for index, byte in enumerate(data):
        if byte == 0x0A:
            lines.append(data[start:index + 1])
            start = index + 1
    if start < len(data):
        lines.append(data[start:])
    return lines


def expected_diff(old, new):
    """Independent line-prefix/suffix diff, not derived from the server."""
    old_lines = split_lines(old)
    new_lines = split_lines(new)
    shortest = min(len(old_lines), len(new_lines))
    prefix = 0
    while prefix < shortest and old_lines[prefix] == new_lines[prefix]:
        prefix += 1
    suffix = 0
    while suffix < shortest - prefix and \
            old_lines[len(old_lines) - 1 - suffix] == \
            new_lines[len(new_lines) - 1 - suffix]:
        suffix += 1
    removed = old_lines[prefix:len(old_lines) - suffix]
    added = new_lines[prefix:len(new_lines) - suffix]
    return {
        "start_line": prefix + 1 if removed or added else 0,
        "removed": [line.decode("utf-8") for line in removed],
        "added": [line.decode("utf-8") for line in added],
    }


def check_preview(value, operation, name, text, source, diff, label="preview"):
    """Validate a preview response; a mutated response is rejected."""
    if not isinstance(value, dict):
        raise AssertionError("%s: preview is not an object: %r" % (label, value))
    assert_exact(set(value), PREVIEW_KEYS, label + " keys")
    assert_exact(value["schema_version"], 1, label + " schema_version")
    assert_exact(value["operation"], operation, label + " operation")
    assert_exact(value["name"], name, label + " name")
    assert_exact(value["source_sha256"], source, label + " source_sha256")
    assert_exact(value["text"], text, label + " text")
    assert_exact(value["diff"], diff, label + " diff")
    return value


def check_commit(value, operation, name, length, source, previous,
                 label="commit"):
    """Validate a committed write response."""
    if not isinstance(value, dict):
        raise AssertionError("%s: commit is not an object: %r" % (label, value))
    assert_exact(set(value), COMMIT_KEYS, label + " keys")
    assert_exact(value["schema_version"], 1, label + " schema_version")
    assert_exact(value["operation"], operation, label + " operation")
    assert_exact(value["name"], name, label + " name")
    assert_exact(value["bytes"], length, label + " bytes")
    assert_exact(value["source_sha256"], source, label + " source_sha256")
    assert_exact(value["previous_source_sha256"], previous,
                 label + " previous_source_sha256")
    return value


# ---------------------------------------------------------- independent image

def decode_v1(path):
    """Decode the on-disk v1 directory without calling production code."""
    raw = Path(path).read_bytes()
    if raw[:8] != MAGIC:
        raise AssertionError("image magic differs at %s" % path)
    if struct.unpack_from("<I", raw, 8)[0] != 1:
        raise AssertionError("image version differs at %s" % path)
    if struct.unpack_from("<I", raw, 12)[0] != BLOCK:
        raise AssertionError("image block size differs at %s" % path)
    capacity = struct.unpack_from("<I", raw, 16)[0]
    if struct.unpack_from("<I", raw, 20)[0] != 63:
        raise AssertionError("image entry capacity differs at %s" % path)
    if capacity * BLOCK != len(raw):
        raise AssertionError("image capacity/length differs at %s" % path)
    files = []
    for slot in range(63):
        entry = raw[64 + slot * 64:128 + slot * 64]
        encoded = entry[:32].split(b"\0", 1)[0]
        if not encoded:
            files.append(None)
            continue
        name = encoded.decode("ascii")
        size, start, count = struct.unpack_from("<III", entry, 32)
        if any(entry[44:64]):
            raise AssertionError("directory entry reserved bytes at %s" % path)
        if count and (start < 1 or start + count > capacity):
            raise AssertionError("directory extent out of range at %s" % path)
        payload = raw[start * BLOCK:start * BLOCK + size]
        if len(payload) != size:
            raise AssertionError("directory file extent differs at %s" % path)
        files.append((name, payload))
    while files and files[-1] is None:
        files.pop()
    return files


# ------------------------------------------------------------ block response

def check_block_list(value, name, data, source, label):
    blocks = reference.scan(data)[0]
    if not isinstance(value, dict):
        raise AssertionError("%s: list is not an object" % label)
    assert_exact(set(value), LIST_KEYS, label + " keys")
    assert_exact(value["schema_version"], 1, label + " schema_version")
    assert_exact(value["operation"], "block_list", label + " operation")
    assert_exact(value["name"], name, label + " name")
    assert_exact(value["source_sha256"], source, label + " source_sha256")
    assert_exact(value["total"], len(blocks), label + " total")
    assert_exact(value["shown"], len(blocks), label + " shown")
    rows = value["blocks"]
    if not isinstance(rows, list):
        raise AssertionError("%s: blocks is not an array" % label)
    assert_exact(len(rows), len(blocks), label + " block count")
    for index, (got, block) in enumerate(zip(rows, blocks)):
        assert_exact(set(got), BLOCK_KEYS, "%s row %d keys" % (label, index))
        assert_exact(got["id"], block["id"], "%s row %d id" % (label, index))
        for key, oracle in (("marker_line", "marker"), ("first_line", "first"),
                            ("last_line", "last"), ("start", "start"),
                            ("end", "end")):
            assert_exact(got[key], block[oracle],
                         "%s row %d %s" % (label, index, key))
    assert_exact([row["id"] for row in rows], [block["id"] for block in blocks],
                 label + " block order")
    return blocks


def check_refs(value, name, data, files, source, label):
    blocks, refs = reference.scan(data)
    del blocks
    if not isinstance(value, dict):
        raise AssertionError("%s: refs is not an object" % label)
    assert_exact(set(value), REFS_KEYS, label + " keys")
    assert_exact(value["schema_version"], 1, label + " schema_version")
    assert_exact(value["operation"], "block_refs", label + " operation")
    assert_exact(value["name"], name, label + " name")
    assert_exact(value["source_sha256"], source, label + " source_sha256")
    assert_exact(value["total"], len(refs), label + " total")
    assert_exact(value["shown"], min(20, len(refs)), label + " shown")
    rows = value["refs"]
    if not isinstance(rows, list):
        raise AssertionError("%s: refs is not an array" % label)
    assert_exact(len(rows), value["shown"], label + " ref count")
    for index, (got, expected) in enumerate(zip(rows, refs)):
        assert_exact(set(got), REF_ROW_KEYS, "%s row %d keys" % (label, index))
        assert_exact(got["source"], name, "%s row %d source" % (label, index))
        assert_exact(got["block_id"], expected["source_id"],
                     "%s row %d block_id" % (label, index))
        for key in ("line", "start", "end"):
            assert_exact(got[key], expected[key],
                         "%s row %d %s" % (label, index, key))
        assert_exact(got["target"], {"name": expected["name"],
                                     "id": expected["id"]},
                     "%s row %d target" % (label, index))
        assert_exact(got["state"],
                     reference.target(files, expected["name"], expected["id"]),
                     "%s row %d state" % (label, index))
    return refs


def check_block_read(value, name, data, block, source, label):
    raw = data[block["start"]:block["end"]]
    if not isinstance(value, dict):
        raise AssertionError("%s: read is not an object" % label)
    assert_exact(set(value), READ_KEYS, label + " keys")
    assert_exact(value["schema_version"], 1, label + " schema_version")
    assert_exact(value["operation"], "block_read", label + " operation")
    assert_exact(value["name"], name, label + " name")
    assert_exact(value["block_id"], block["id"], label + " block_id")
    assert_exact(value["source_sha256"], source, label + " source_sha256")
    assert_exact(value["text"].encode("utf-8"), raw, label + " raw text")
    assert_exact(value["complete"], True, label + " complete")
    assert_exact(value["next_offset"], None, label + " next_offset")
    assert_exact(value["block"], {"start": block["start"], "end": block["end"],
                                  "first_line": block["first"], "last_line":
                                  block["last"]}, label + " block range")
    assert_exact(value["page"], {"start": block["start"], "end": block["end"],
                                 "first_line": block["first"], "last_line":
                                 block["last"]}, label + " page range")
    return value["text"]


def check_error(server, tool, arguments, code, label):
    value, error = server.call(tool, arguments)
    if value is not None:
        raise AssertionError("%s: unexpected success %r" % (label, value))
    assert_exact(error, {"code": code}, label + " error")
    return error


# --------------------------------------------------------------- fixtures/delta

def load_fixtures(source):
    raw = {relative: (source / relative).read_bytes()
           for relative in FIXTURES}
    return {
        "initial": raw[FIXTURES[0]],
        "updated": raw[FIXTURES[1]],
        "ordinary": {name: raw["tests/learning-note/notes/" + name]
                     for name in ORDINARY},
    }


def check_fixture_labels(initial):
    header = parse_header(initial)
    assert_exact(header["present"], True, "fixture header present")
    assert_exact(header["tags"], ["learning"], "fixture header tags")
    blocks, refs = reference.scan(initial)
    assert_exact([block["id"] for block in blocks], list(LEARNING_BLOCKS),
                 "fixture block order")
    for token in (b"@yan-note 1\n", b"tag: learning\n", b"@end\n",
                  "示例夹具".encode("utf-8"), "模拟信息".encode("utf-8"),
                  "未进行真实核对".encode("utf-8"),
                  "不代表闫博的理解".encode("utf-8")):
        if token not in initial:
            raise AssertionError("fixture does not label itself: %r" % token)
    for token in ("已理解".encode("utf-8"), "已学会".encode("utf-8"),
                  "已掌握".encode("utf-8"), "真实理解".encode("utf-8")):
        if token in initial:
            raise AssertionError("fixture claims real understanding: %r" % token)
    return header, blocks, refs


def _body(data, block):
    return data[block["start"]:block["end"]]


def check_block_delta(initial, updated):
    """Exactly one unknown line moved to known and one log entry appended."""
    initial_header = parse_header(initial)
    updated_header = parse_header(updated)
    assert_exact(updated[:updated_header["header_length"]],
                 initial[:initial_header["header_length"]],
                 "updated header bytes")
    initial_blocks = {block["id"]: block for block in reference.scan(initial)[0]}
    updated_blocks = {block["id"]: block for block in reference.scan(updated)[0]}
    assert_exact(list(updated_blocks), list(initial_blocks),
                 "updated block order")

    assert_exact(_body(initial, initial_blocks["known"]), b"",
                 "initial known body")
    assert_exact(_body(updated, updated_blocks["known"]), MOVED_LINE,
                 "updated known body")
    assert_exact(_body(initial, initial_blocks["unknown"]),
                 MOVED_LINE + PLIC_LINE, "initial unknown body")
    assert_exact(_body(updated, updated_blocks["unknown"]), PLIC_LINE,
                 "updated unknown body")
    for identity in ("criteria", "untouched"):
        assert_exact(_body(updated, updated_blocks[identity]),
                     _body(initial, initial_blocks[identity]),
                     identity + " body")
    initial_log = _body(initial, initial_blocks["log"])
    updated_log = _body(updated, updated_blocks["log"])
    assert_exact(updated_log, initial_log + LOG_ENTRY, "log append")
    assert_exact(updated_log.count(LOG_ENTRY), 1, "one appended log entry")

    # Rebuild the whole updated file from the initial bytes using only the two
    # declared changes.  Any other byte difference makes this fail.
    header = initial[:initial_header["header_length"]]
    body = initial[initial_header["header_length"]:]
    assert_exact(body.count(MOVED_LINE), 1, "moved line occurs once")
    body = body.replace(MOVED_LINE, b"", 1)
    marker = b"@block known\n"
    assert_exact(body.count(marker), 1, "known marker occurs once")
    body = body.replace(marker, marker + MOVED_LINE, 1)
    assert_exact(updated, header + body + LOG_ENTRY,
                 "updated fixture is exactly the two changes")


def simulate_owner_confirmation(preview, operation, name, text, source, diff,
                                label):
    """TEST-ONLY stand-in for an explicit owner decision.

    No human is asked here.  This function only validates the preview and
    returns its identity; it is not evidence of, and must never be cited as,
    a real owner judgment.
    """
    print("SIMULATED owner confirmation (%s): test-only, not a real human "
          "judgment" % label, flush=True)
    check_preview(preview, operation, name, text, source, diff,
                  label + " confirmed preview")
    return preview["source_sha256"]


# --------------------------------------------------------------- input checks

def require_inputs(source, run, guest, server):
    missing = []
    for relative in INPUTS:
        path = source / relative
        if not path.is_file():
            missing.append(path)
    for flag, path in (("--run", run), ("--guest", guest), ("--server", server)):
        if path is None:
            missing.append(flag)
        elif not Path(path).is_file():
            missing.append(Path(path))
    return missing


def assert_dependencies_absent(image_path):
    for suffix in (".index", ".model", ".runner"):
        path = Path(str(image_path) + suffix)
        if path.exists():
            raise AssertionError("learning dependency must be absent: %s" % path)


def seed_ordinary(root, fixtures):
    """One ordinary library shared by AC1 and AC2."""
    ordinary_files = [(name, fixtures["ordinary"][name]) for name in ORDINARY]
    ordinary_image = root / "ordinary.img"
    ordinary_image.write_bytes(image(ordinary_files, capacity=16))
    return ordinary_image, logical_hash(ordinary_files)


# --------------------------------------------------------------------- AC1

def run_ac1(args, root, fixtures, ordinary_image, ordinary_sha,
            ordinary_identity):
    initial = fixtures["initial"]
    learning_files = [(LEARNING, initial)]
    learning_image = root / "ac1-learning.img"
    learning_image.write_bytes(image(learning_files, capacity=16))
    learning_identity = logical_hash(learning_files)
    learning_sha = digest(learning_image)

    header, expected_blocks, expected_refs = check_fixture_labels(initial)
    assert_exact([block["id"] for block in expected_blocks],
                 list(LEARNING_BLOCKS), "example learning blocks")
    assert_exact(header["tags"], ["learning"], "example learning tag")

    learning = _mcp_server(args, learning_image, allow_write=True)
    ordinary = _mcp_server(args, ordinary_image, allow_write=False)
    try:
        listed, error = learning.call("yanos_block_list", {"name": LEARNING})
        assert_exact(error, None, "learning block_list error")
        blocks = check_block_list(listed, LEARNING, initial, learning_identity,
                                  "learning block_list")
        assert_exact(len(blocks), 5, "learning block_list count")
        assert_exact(listed["source_sha256"], logical_hash(learning_files),
                     "learning block_list identity")

        refs_value, error = learning.call("yanos_block_refs", {"name": LEARNING})
        assert_exact(error, None, "learning block_refs error")
        refs = check_refs(refs_value, LEARNING, initial, {LEARNING: initial},
                          learning_identity, "learning block_refs")
        assert_exact(refs, expected_refs, "learning refs oracle")
        assert_exact([row["state"] for row in refs_value["refs"]],
                     ["DANGLING_FILE"] * len(refs),
                     "learning refs are dangling files")

        # No cross-library resolution in either direction.
        for ref in refs:
            check_error(learning, "yanos_block_read",
                        {"name": ref["name"], "block_id": ref["id"]},
                        "NOT_FOUND", "learning reads " + ref["name"])
        for name in ORDINARY:
            check_error(learning, "yanos_block_list", {"name": name},
                        "NOT_FOUND", "learning lists " + name)
        check_error(ordinary, "yanos_block_read",
                    {"name": LEARNING, "block_id": "log"}, "NOT_FOUND",
                    "ordinary reads learning note")
        check_error(ordinary, "yanos_block_list", {"name": LEARNING},
                    "NOT_FOUND", "ordinary lists learning note")

        # Ordinary instance is the independent source oracle for every target.
        for ref in refs:
            ordinary_data = fixtures["ordinary"][ref["name"]]
            ordinary_blocks = reference.scan(ordinary_data)[0]
            block = next((item for item in ordinary_blocks
                          if item["id"] == ref["id"]), None)
            if block is None:
                raise AssertionError("ordinary oracle lacks %r" % (ref,))
            value, error = ordinary.call(
                "yanos_block_read",
                {"name": ref["name"], "block_id": ref["id"]})
            assert_exact(error, None, "ordinary read " + ref["name"])
            check_block_read(value, ref["name"], ordinary_data, block,
                             ordinary_identity,
                             "ordinary read " + ref["name"] + "#" + ref["id"])

        assert_exact(digest(ordinary_image), ordinary_sha,
                     "ordinary image after AC1 reads")
        assert_exact(logical_hash(decode_v1(ordinary_image)),
                     ordinary_identity, "ordinary identity after AC1 reads")
        assert_exact(digest(learning_image), learning_sha,
                     "learning image after AC1 reads")
        assert_exact(logical_hash(decode_v1(learning_image)),
                     learning_identity, "learning identity after AC1 reads")
    finally:
        try:
            ordinary.close()
        finally:
            learning.close()


# --------------------------------------------------------------------- AC2

def run_ac2(args, root, fixtures, ordinary_image, ordinary_sha,
            ordinary_identity):
    initial = fixtures["initial"]
    updated = fixtures["updated"]
    created_identity = logical_hash([(LEARNING, initial)])
    updated_identity = logical_hash([(LEARNING, updated)])
    learning_image = root / "ac2-learning.img"
    learning_image.write_bytes(image([], capacity=16))

    empty_identity = logical_hash([])
    assert_exact(empty_identity, hashlib.sha256(b"").hexdigest(),
                 "empty image identity")
    empty_sha = digest(learning_image)
    empty_mtime = mtime_ns(learning_image)
    assert_dependencies_absent(learning_image)

    lock = Path(os.path.realpath(learning_image) + ".lock")
    lock_existed = lock.exists()
    lock_bytes = lock.read_bytes() if lock_existed else b""
    lock_mtime = lock.stat().st_mtime_ns if lock_existed else None

    ordinary = _mcp_server(args, ordinary_image, allow_write=False)

    def check_ordinary(label, with_server=True):
        assert_exact(digest(ordinary_image), ordinary_sha,
                     "ordinary image " + label)
        assert_exact(logical_hash(decode_v1(ordinary_image)),
                     ordinary_identity, "ordinary identity " + label)
        if with_server:
            assert_exact(ordinary.identity(ORDINARY[0]), ordinary_identity,
                         "ordinary MCP identity " + label)

    try:
        learning = _mcp_server(args, learning_image, allow_write=True)
        try:
            check_ordinary("before create")
            assert_exact(digest(learning_image), empty_sha,
                         "learning image before create")
            assert_exact(mtime_ns(learning_image), empty_mtime,
                         "learning mtime before create")

            before_preview = learning_image.read_bytes()
            before_preview_mtime = mtime_ns(learning_image)
            create_preview, error = learning.call(
                "yanos_note_create",
                {"name": LEARNING, "text": initial.decode("utf-8"),
                 "dry_run": True})
            assert_exact(error, None, "create preview error")
            check_preview(create_preview, "note_create_preview", LEARNING,
                          initial.decode("utf-8"), empty_identity,
                          expected_diff(b"", initial), "create preview")
            assert_exact(learning_image.read_bytes(), before_preview,
                         "learning image after create preview")
            assert_exact(mtime_ns(learning_image), before_preview_mtime,
                         "learning mtime after create preview")
            if lock_existed:
                assert_exact(lock_bytes, b"", "existing lock bytes")
                assert_exact(lock.read_bytes(), b"", "lock bytes after preview")
                assert_exact(mtime_ns(lock), lock_mtime,
                             "lock mtime after preview")
            else:
                assert_exact(lock.exists(), True,
                             "preview may create the side lock")
                assert_exact(lock.read_bytes(), b"",
                             "created lock is empty")
            check_ordinary("after create preview")

            create_source = simulate_owner_confirmation(
                create_preview, "note_create_preview", LEARNING,
                initial.decode("utf-8"), empty_identity,
                expected_diff(b"", initial), "create")
            assert_exact(create_source, empty_identity,
                         "create preview identity")

            created, error = learning.call(
                "yanos_note_create",
                {"name": LEARNING, "text": initial.decode("utf-8"),
                 "expected_source": create_source})
            assert_exact(error, None, "create commit error")
            created_files = decode_v1(learning_image)
            assert_exact(logical_hash(created_files), created_identity,
                         "on-disk create identity")
            check_commit(created, "note_create", LEARNING, len(initial),
                         created_identity, None, "create commit")
            assert_exact(created_files, [(LEARNING, initial)],
                         "on-disk bytes after create")
            if digest(learning_image) == empty_sha:
                raise AssertionError("create did not change the image SHA")
            created_sha = digest(learning_image)
            check_ordinary("after create commit")

            before_replace = learning_image.read_bytes()
            before_replace_mtime = mtime_ns(learning_image)
            if not lock.exists():
                raise AssertionError("replace preview requires the side lock")
            before_replace_lock = lock.read_bytes()
            before_replace_lock_mtime = mtime_ns(lock)
            replace_preview, error = learning.call(
                "yanos_note_replace",
                {"name": LEARNING, "text": updated.decode("utf-8"),
                 "dry_run": True})
            assert_exact(error, None, "replace preview error")
            replace_diff = expected_diff(initial, updated)
            check_preview(replace_preview, "note_replace_preview", LEARNING,
                          updated.decode("utf-8"), created_identity,
                          replace_diff, "replace preview")
            assert_exact(replace_preview["source_sha256"],
                         created["source_sha256"],
                         "replace preview identity")
            assert_exact(learning_image.read_bytes(), before_replace,
                         "learning image after replace preview")
            assert_exact(mtime_ns(learning_image), before_replace_mtime,
                         "learning mtime after replace preview")
            assert_exact(before_replace_lock, b"",
                         "side lock bytes before replace preview")
            assert_exact(lock.read_bytes(), before_replace_lock,
                         "side lock bytes after replace preview")
            assert_exact(mtime_ns(lock), before_replace_lock_mtime,
                         "side lock mtime after replace preview")
            check_ordinary("after replace preview")

            check_block_delta(initial, updated)

            replace_source = simulate_owner_confirmation(
                replace_preview, "note_replace_preview", LEARNING,
                updated.decode("utf-8"), created_identity, replace_diff,
                "replace")
            assert_exact(replace_source, replace_preview["source_sha256"],
                         "replace preview identity")

            # A rejected commit must leave the image untouched, so the closed
            # loop below starts from exactly the previewed bytes. The stale
            # identity cannot match the note that now exists, and STALE is
            # decided before every per-note check (0035-R10 ordering).
            stale_source = "0" * 64
            check_error(learning, "yanos_note_replace",
                        {"name": LEARNING, "text": updated.decode("utf-8"),
                         "expected_source": stale_source},
                        "STALE", "replace commit with stale identity")
            assert_exact(learning_image.read_bytes(), before_replace,
                         "learning image after stale replace")
            assert_exact(decode_v1(learning_image), created_files,
                         "on-disk bytes after stale replace")

            replaced, error = learning.call(
                "yanos_note_replace",
                {"name": LEARNING, "text": updated.decode("utf-8"),
                 "expected_source": replace_source})
            assert_exact(error, None, "replace commit error")
            replaced_files = decode_v1(learning_image)
            assert_exact(logical_hash(replaced_files), updated_identity,
                         "on-disk replace identity")
            check_commit(replaced, "note_replace", LEARNING, len(updated),
                         updated_identity, replace_source, "replace commit")
            assert_exact(replaced["previous_source_sha256"], replace_source,
                         "replace previous identity")
            assert_exact(replaced["source_sha256"], updated_identity,
                         "replace post identity")
            assert_exact(replaced_files, [(LEARNING, updated)],
                         "on-disk bytes after replace")
            if digest(learning_image) == created_sha:
                raise AssertionError("replace did not change the image SHA")
            check_ordinary("after replace commit")
        finally:
            learning.close()

        # Restart a fresh MCP process on the same persisted learning image.
        learning = _mcp_server(args, learning_image, allow_write=True)
        try:
            assert_dependencies_absent(learning_image)
            listed, error = learning.call("yanos_block_list", {"name": LEARNING})
            assert_exact(error, None, "restart block_list error")
            blocks = check_block_list(listed, LEARNING, updated,
                                      updated_identity, "restart block_list")
            assert_exact([block["id"] for block in blocks],
                         list(LEARNING_BLOCKS), "restart block ids")
            for block in blocks:
                value, error = learning.call(
                    "yanos_block_read",
                    {"name": LEARNING, "block_id": block["id"]})
                assert_exact(error, None,
                             "restart read " + block["id"] + " error")
                check_block_read(value, LEARNING, updated, block,
                                 updated_identity,
                                 "restart read " + block["id"])

            refs_value, error = learning.call("yanos_block_refs",
                                              {"name": LEARNING})
            assert_exact(error, None, "restart block_refs error")
            refs = check_refs(refs_value, LEARNING, updated,
                              {LEARNING: updated}, updated_identity,
                              "restart block_refs")
            assert_exact([row["state"] for row in refs_value["refs"]],
                         ["DANGLING_FILE"] * len(refs),
                         "restart refs stay dangling")
            check_error(learning, "yanos_search", {"query": "cpu"},
                        "DEPENDENCY", "learning search without dependencies")
            check_ordinary("after restart reads")
        finally:
            learning.close()
    finally:
        ordinary.close()

    # Both MCP instances are closed before the real Guest starts.
    guest_files = {LEARNING: updated}
    expectations = [
        (b"block list " + LEARNING.encode(),
         reference.query(guest_files, b"block list " + LEARNING.encode())),
        (b"cat " + LEARNING.encode(),
         base.display(updated) + b"\r\nOK cat\r\n"),
        (b"block show " + LEARNING.encode() + b"#log",
         reference.query(guest_files,
                         b"block show " + LEARNING.encode() + b"#log")),
    ]
    guest_work = root / "guest"
    guest_work.mkdir(parents=True, exist_ok=True)
    before_guest = learning_image.read_bytes()
    guest_disk, guest_result = run_search_terms.run_commands(
        args, guest_work, "learning-note-guest", [], expectations,
        readonly=True, disk=learning_image)
    assert_exact(guest_disk, learning_image, "Guest disk")
    base.require_fragment(guest_result, b"yanfs: exit\r\n",
                          "learning-note Guest exit")
    assert_exact(learning_image.read_bytes(), before_guest,
                 "learning image after read-only Guest")
    assert_exact(decode_v1(learning_image), [(LEARNING, updated)],
                 "on-disk updated bytes after Guest")
    check_ordinary("after Guest", with_server=False)


# -------------------------------------------------------------------- driver

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="0035-R8-AC1/AC2 learning-note Host/Guest acceptance")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    # Validated by require_inputs so a missing value is the required exit 1
    # "missing required input" failure, not an argparse usage exit 2.
    parser.add_argument("--run", type=Path)
    parser.add_argument("--guest", type=Path)
    parser.add_argument("--server", type=Path)
    args = parser.parse_args(argv)

    missing = require_inputs(args.source, args.run, args.guest, args.server)
    if missing:
        for path in missing:
            print(":FAIL: missing required input: %s" % path)
        return 1

    try:
        fixtures = load_fixtures(args.source)
        args.work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=str(args.work),
                                         prefix="yan-learning-note-") as tmp:
            root = Path(tmp)
            ordinary_image, ordinary_identity = seed_ordinary(root, fixtures)
            ordinary_sha = digest(ordinary_image)
            run_ac1(args, root, fixtures, ordinary_image, ordinary_sha,
                    ordinary_identity)
            run_ac2(args, root, fixtures, ordinary_image, ordinary_sha,
                    ordinary_identity)
    except AssertionError as error:
        print(":FAIL:", error)
        return 1
    except (reference.BlockError, ValueError, KeyError) as error:
        print(":FAIL:", error)
        return 1
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        print(":HARNESS-ERROR:", error)
        return 2
    except Exception as error:  # noqa: BLE001 - never report a crash as a pass
        print(":HARNESS-ERROR:", repr(error))
        return 2
    print("PASS 0035-R8-AC1/AC2 learning note: fixture, two-image reads, "
          "preview/commit, restart, Guest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
