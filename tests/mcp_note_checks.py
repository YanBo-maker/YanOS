#!/usr/bin/env python3
"""0035 R3/R4/R5/R6/R10/R11 checks against real yan_mcp processes."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from mcp_checks import Wire, unique_object
from semantic_fixture import image

CASES = 0


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def lock_path(image_path):
    return Path(os.path.realpath(image_path) + ".lock")


def make_image(path, files, capacity=None):
    Path(path).write_bytes(image(files, capacity))
    return Path(path)


class Server:
    def __init__(self, server, image_path, allow_write=True, env=None):
        self.image = Path(image_path)
        self.ident = 0
        args = [str(server), "--disk-image", str(self.image),
                "--index", str(self.image) + ".index",
                "--model", str(self.image) + ".model",
                "--runner", str(self.image) + ".runner"]
        if allow_write:
            args.append("--allow-write")
        self.wire = Wire(args, dict(os.environ, **(env or {})))

    def call(self, name, arguments):
        self.ident += 1
        self.wire.send("tools/call", ident=self.ident,
                       params={"name": name, "arguments": arguments})
        reply = self.wire.get()
        assert reply["id"] == self.ident, "typed id"
        result = reply["result"]
        assert len(result["content"]) == 1 and result["content"][0]["type"] == "text"
        value = json.loads(result["content"][0]["text"],
                           object_pairs_hook=unique_object)
        if result["isError"]:
            assert "structuredContent" not in result
            return None, value
        assert value == result["structuredContent"], "text mirrors structuredContent"
        return value, None

    def identity(self, name):
        value, error = self.call("yanos_block_list", {"name": name})
        assert error is None, ("block_list", error)
        return value["source_sha256"]

    def close(self):
        self.wire.close()


def expect(value, error, code, committed=False):
    assert value is None, ("unexpected success", value)
    assert error.get("code") == code, (code, error)
    assert error.get("committed", False) is committed, ("committed", error)


def probe(probe_path, image_path, name):
    run = subprocess.run([str(probe_path), str(image_path), name],
                         capture_output=True, timeout=15)
    if run.returncode != 0:
        raise RuntimeError(("note_read_probe", run.returncode, run.stderr))
    return run.stdout


def test_r3(server, root, probe_path):
    global CASES
    source = make_image(root / "r3.img", [("seed.txt", b"seed\n")], 16)
    wire = Server(server, source)
    try:
        expected = wire.identity("seed.txt")
        text = "@block greeting\nhello\n"
        value, error = wire.call("yanos_note_create", {
            "name": "a.txt", "text": text, "expected_source": expected})
        assert error is None and value["operation"] == "note_create", ("create", error)
        assert value["name"] == "a.txt" and value["bytes"] == len(text.encode())
        assert value["previous_source_sha256"] is None
        assert value["source_sha256"] == wire.identity("a.txt"), "post identity"
        assert probe(probe_path, source, "a.txt") == text.encode(), "raw readback"
        block, error = wire.call("yanos_block_read",
                                 {"name": "a.txt", "block_id": "greeting"})
        assert error is None and block["text"] == "hello\n", "named block"
        before = digest(source)
        _, error = wire.call("yanos_note_create", {
            "name": "a.txt", "text": "other\n",
            "expected_source": value["source_sha256"]})
        expect(None, error, "EXISTS")
        assert digest(source) == before, "EXISTS keeps image"
        before = digest(source)
        again, error = wire.call("yanos_note_create", {
            "name": "a.txt", "text": text, "expected_source": expected})
        assert error is None and again.get("unchanged") is True, ("unchanged", error)
        assert digest(source) == before, "unchanged keeps image"
        CASES += 1
    finally:
        wire.close()

    full = [(f"f{i:02d}.txt", b"x") for i in range(63)]
    source = make_image(root / "r3full.img", full, 64)
    wire = Server(server, source)
    try:
        before = digest(source)
        _, error = wire.call("yanos_note_create", {
            "name": "overflow.txt", "text": "x\n",
            "expected_source": wire.identity("f00.txt")})
        expect(None, error, "DIRECTORY_FULL")
        assert digest(source) == before, "DIRECTORY_FULL keeps image"
        CASES += 1
    finally:
        wire.close()

    source = make_image(root / "r3space.img", [("big.txt", b"a" * 4096)], 2)
    wire = Server(server, source)
    try:
        before = digest(source)
        _, error = wire.call("yanos_note_create", {
            "name": "second.txt", "text": "x\n",
            "expected_source": wire.identity("big.txt")})
        expect(None, error, "NOSPACE")
        assert digest(source) == before, "NOSPACE keeps image"
        CASES += 1
    finally:
        wire.close()


def test_r4(server, root, probe_path):
    global CASES
    source = make_image(root / "r4.img", [("a.txt", b"old\n")], 16)
    wire = Server(server, source)
    try:
        expected = wire.identity("a.txt")
        text = "new\n"
        value, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": text, "expected_source": expected})
        assert error is None and value["operation"] == "note_replace", ("replace", error)
        assert value["previous_source_sha256"] == expected
        assert value["source_sha256"] == wire.identity("a.txt")
        assert probe(probe_path, source, "a.txt") == text.encode()
        before = digest(source)
        again, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": text, "expected_source": expected})
        assert error is None and again.get("unchanged") is True, ("replace unchanged", error)
        assert digest(source) == before

        expected = wire.identity("a.txt")
        created, error = wire.call("yanos_note_create", {
            "name": "b.txt", "text": "b\n", "expected_source": expected})
        assert error is None, ("seed write", error)
        before = digest(source)
        _, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": "stale\n", "expected_source": expected})
        expect(None, error, "STALE")
        assert digest(source) == before, "STALE keeps image"
        CASES += 1
    finally:
        wire.close()

    missing = root / "r4missing.img"
    wire = Server(server, missing)
    try:
        for arguments in ({"name": "a.txt", "text": "x\n"},
                          {"name": "a.txt", "text": "x\n", "expected_source": "BAD"},
                          {"name": "a.txt", "text": "x\n", "expected_source": "0" * 63}):
            _, error = wire.call("yanos_note_create", arguments)
            expect(None, error, "INVALID")
            _, error = wire.call("yanos_note_replace", arguments)
            expect(None, error, "INVALID")
        assert not missing.exists() and not lock_path(missing).exists(), "zero source I/O"
        CASES += 1
    finally:
        wire.close()


def diff_of(value):
    return value["diff"]


def test_r10(server, root):
    global CASES
    files = [("x.txt", b"x\ny\nz\n"), ("a.txt", b"a"),
             ("same.txt", b"same\n"), ("bad.txt", b"\xff\n"),
             ("m.txt", b"old\n")]
    source = make_image(root / "r10.img", files, 16)
    wire = Server(server, source)
    try:
        value, error = wire.call("yanos_note_replace", {
            "name": "x.txt", "text": "x\nY\nz\n", "dry_run": True})
        assert error is None and value["operation"] == "note_replace_preview"
        assert diff_of(value) == {"start_line": 2, "removed": ["y\n"],
                                  "added": ["Y\n"]}, diff_of(value)
        value, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": "a\n", "dry_run": True})
        assert error is None and diff_of(value) == {
            "start_line": 1, "removed": ["a"], "added": ["a\n"]}
        value, error = wire.call("yanos_note_create", {
            "name": "q.txt", "text": "q\n", "dry_run": True})
        assert error is None and diff_of(value) == {
            "start_line": 1, "removed": [], "added": ["q\n"]}
        value, error = wire.call("yanos_note_replace", {
            "name": "same.txt", "text": "same\n", "dry_run": True})
        assert error is None and diff_of(value) == {
            "start_line": 0, "removed": [], "added": []}, diff_of(value)
        _, error = wire.call("yanos_note_replace", {
            "name": "bad.txt", "text": "fixed\n", "dry_run": True})
        expect(None, error, "INVALID_TEXT")
        _, error = wire.call("yanos_note_create", {
            "name": "x.txt", "text": "x\n", "dry_run": True})
        expect(None, error, "EXISTS")
        _, error = wire.call("yanos_note_replace", {
            "name": "missing.txt", "text": "x\n", "dry_run": True})
        expect(None, error, "NOT_FOUND")
        _, error = wire.call("yanos_note_replace", {
            "name": "x.txt", "text": "x\nY\nz\n",
            "expected_source": "0" * 64, "dry_run": True})
        expect(None, error, "STALE")
        CASES += 1

        lock = lock_path(source)
        lock.touch()
        before_image, before_lock = digest(source), digest(lock)
        before_image_mtime = os.stat(source).st_mtime_ns
        before_lock_mtime = os.stat(lock).st_mtime_ns
        value, error = wire.call("yanos_note_replace", {
            "name": "m.txt", "text": "new\n", "dry_run": True})
        assert error is None and value["source_sha256"] == wire.identity("m.txt")
        assert digest(source) == before_image and digest(lock) == before_lock
        assert os.stat(source).st_mtime_ns == before_image_mtime
        assert os.stat(lock).st_mtime_ns == before_lock_mtime
        with open(lock, "r+b") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            _, error = wire.call("yanos_note_replace", {
                "name": "m.txt", "text": "new\n", "dry_run": True})
            expect(None, error, "BUSY")
            fcntl.flock(held.fileno(), fcntl.LOCK_UN)
        with open(lock, "r+b") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_SH | fcntl.LOCK_NB)
            _, error = wire.call("yanos_note_replace", {
                "name": "m.txt", "text": "new\n", "dry_run": True})
            assert error is None, ("shared lock preview", error)
        CASES += 1

        preview, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": "A\n", "dry_run": True})
        assert error is None
        value, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": preview["text"],
            "expected_source": preview["source_sha256"]})
        assert error is None and value["operation"] == "note_replace"
        CASES += 1

        preview, error = wire.call("yanos_note_replace", {
            "name": "same.txt", "text": "changed\n", "dry_run": True})
        assert error is None
        _, error = wire.call("yanos_note_create", {
            "name": "other.txt", "text": "o\n",
            "expected_source": preview["source_sha256"]})
        assert error is None
        before = digest(source)
        _, error = wire.call("yanos_note_replace", {
            "name": "same.txt", "text": "changed\n",
            "expected_source": preview["source_sha256"]})
        expect(None, error, "STALE")
        assert digest(source) == before, "R10-AC4 zero write"
        CASES += 1
    finally:
        wire.close()
    return source


def test_r10_ac3_readback(server, root, probe_path, source):
    global CASES
    wire = Server(server, source)
    try:
        value = None
        preview, error = wire.call("yanos_note_replace", {
            "name": "x.txt", "text": "x\nlast\n", "dry_run": True})
        assert error is None
        value, error = wire.call("yanos_note_replace", {
            "name": "x.txt", "text": preview["text"],
            "expected_source": preview["source_sha256"]})
        assert error is None
        assert probe(probe_path, source, "x.txt") == preview["text"].encode()
        CASES += 1
    finally:
        wire.close()


def test_r6_committed(server, root, probe_path, fault_lib):
    global CASES
    source = make_image(root / "r6.img", [("a.txt", b"old\n")], 16)
    wire = Server(server, source, env={
        "LD_PRELOAD": str(fault_lib), "YAN_TEST_DIR_FSYNC_FAULT": "1"})
    try:
        expected = wire.identity("a.txt")
        value, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": "new\n", "expected_source": expected})
        expect(value, error, "IO", committed=True)
        assert probe(probe_path, source, "a.txt") == b"new\n", "renamed before fsync"
        CASES += 1
    finally:
        wire.close()

    source = make_image(root / "r6corrupt.img", [("a.txt", b"old\n")], 16)
    data = bytearray(Path(source).read_bytes())
    data[60] ^= 0xFF
    Path(source).write_bytes(data)
    wire = Server(server, source)
    try:
        before = digest(source)
        _, error = wire.call("yanos_note_replace", {
            "name": "a.txt", "text": "new\n", "expected_source": "0" * 64})
        expect(None, error, "CORRUPT")
        assert digest(source) == before
        CASES += 1
    finally:
        wire.close()


def wait_for_lock_holder(path, process):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        assert process.poll() is None, ("yan_run exited", process.returncode)
        with open(path, "a+") as held:
            try:
                fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return
            else:
                fcntl.flock(held.fileno(), fcntl.LOCK_UN)
        time.sleep(0.02)
    raise RuntimeError("yan_run never acquired the side lock")


def test_r5(server, root, yan_run, guest_elf):
    global CASES
    source = make_image(root / "r5.img", [("seed.txt", b"seed\n")], 16)
    wire = Server(server, source)
    try:
        expected = wire.identity("seed.txt")
        lock = lock_path(source)
        lock.touch()
        before = digest(source)
        with open(lock, "r+b") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            started = time.monotonic()
            _, error = wire.call("yanos_note_create", {
                "name": "busy.txt", "text": "x\n", "expected_source": expected})
            elapsed = time.monotonic() - started
            expect(None, error, "BUSY")
            assert elapsed < 2.0, ("BUSY waited", elapsed)
        assert digest(source) == before
        CASES += 1
    finally:
        wire.close()

    wire = Server(server, source)
    guest = subprocess.Popen(
        [str(yan_run), "--image", str(guest_elf), "--disk-image", str(source),
         "--terminal", "--max-steps", "1000000000"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for_lock_holder(lock_path(source), guest)
        expected = wire.identity("seed.txt")
        before = digest(source)
        _, error = wire.call("yanos_note_create", {
            "name": "guest.txt", "text": "x\n", "expected_source": expected})
        expect(None, error, "BUSY")
        assert digest(source) == before
        value, error = wire.call("yanos_block_list", {"name": "seed.txt"})
        assert error is None and value["source_sha256"] == expected, "R5-AC5"
        CASES += 1
    finally:
        guest.terminate()
        try:
            guest.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.communicate()
        wire.close()


def test_r11(server, root):
    global CASES
    notes = make_image(root / "notes.img", [("seed.txt", b"notes\n")], 16)
    learning = make_image(root / "learning.img", [("seed.txt", b"learn\n")], 16)
    notes_wire = Server(server, notes)
    learning_wire = Server(server, learning)
    try:
        notes_identity = notes_wire.identity("seed.txt")
        learning_identity = learning_wire.identity("seed.txt")
        notes_hash = digest(notes)
        value, error = learning_wire.call("yanos_note_create", {
            "name": "learn.txt", "text": "l\n", "expected_source": learning_identity})
        assert error is None
        assert notes_wire.identity("seed.txt") == notes_identity
        assert digest(notes) == notes_hash, "learning write keeps notes.img bytes"
        assert learning_wire.identity("seed.txt") != learning_identity
        learning_hash = digest(learning)
        notes_identity = notes_wire.identity("seed.txt")
        learning_identity = learning_wire.identity("seed.txt")
        value, error = notes_wire.call("yanos_note_create", {
            "name": "note.txt", "text": "n\n", "expected_source": notes_identity})
        assert error is None
        assert digest(learning) == learning_hash, "notes write keeps learning.img bytes"
        assert learning_wire.identity("seed.txt") == learning_identity
        CASES += 1

        with open(lock_path(learning), "r+b") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            value, error = notes_wire.call("yanos_note_create", {
                "name": "cross.txt", "text": "c\n",
                "expected_source": notes_wire.identity("seed.txt")})
            assert error is None, ("notes independent", error)
            _, error = learning_wire.call("yanos_note_create", {
                "name": "blocked.txt", "text": "b\n",
                "expected_source": learning_wire.identity("seed.txt")})
            expect(None, error, "BUSY")
        with open(lock_path(notes), "r+b") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            value, error = learning_wire.call("yanos_note_create", {
                "name": "cross2.txt", "text": "c\n",
                "expected_source": learning_wire.identity("seed.txt")})
            assert error is None, ("learning independent", error)
            _, error = notes_wire.call("yanos_note_create", {
                "name": "blocked2.txt", "text": "b\n",
                "expected_source": notes_wire.identity("seed.txt")})
            expect(None, error, "BUSY")
        CASES += 1
    finally:
        notes_wire.close()
        learning_wire.close()

    shared = make_image(root / "shared.img", [("seed.txt", b"s\n")], 16)
    first = Server(server, shared)
    second = Server(server, shared)
    try:
        lock_path(shared).touch()
        with open(lock_path(shared), "r+b") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            for wire in (first, second):
                _, error = wire.call("yanos_note_create", {
                    "name": "same.txt", "text": "x\n",
                    "expected_source": wire.identity("seed.txt")})
                expect(None, error, "BUSY")
        CASES += 1
    finally:
        first.close()
        second.close()

    lonely = make_image(root / "learning-alone.img", [("seed.txt", b"l\n")], 16)
    wire = Server(server, lonely)
    try:
        value, error = wire.call("yanos_block_list", {"name": "seed.txt"})
        assert error is None and value["source_sha256"] == wire.identity("seed.txt")
        value, error = wire.call("yanos_note_create", {
            "name": "learn.txt", "text": "l\n",
            "expected_source": wire.identity("seed.txt")})
        assert error is None
        _, error = wire.call("yanos_search", {"query": "cpu"})
        expect(None, error, "DEPENDENCY")
        CASES += 1
    finally:
        wire.close()


def main():
    if len(sys.argv) != 6:
        print("usage: mcp_note_checks.py YAN_MCP YAN_RUN GUEST_ELF "
              "NOTE_READ_PROBE FSYNC_FAULT_LIB", file=sys.stderr)
        return 2
    server, yan_run, guest_elf, probe_path, fault_lib = [
        Path(item).resolve() for item in sys.argv[1:]]
    for path in (server, yan_run, guest_elf, probe_path, fault_lib):
        if not path.is_file():
            raise RuntimeError("FAIL fixture missing: " + str(path))
    with tempfile.TemporaryDirectory(prefix="yan-mcp-note-") as directory:
        root = Path(directory)
        test_r3(server, root, probe_path)
        test_r4(server, root, probe_path)
        source = test_r10(server, root)
        test_r10_ac3_readback(server, root, probe_path, source)
        test_r6_committed(server, root, probe_path, fault_lib)
        test_r5(server, root, yan_run, guest_elf)
        test_r11(server, root)
    print("PASS 0035 note checks groups=" + str(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
