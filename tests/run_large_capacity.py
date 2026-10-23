#!/usr/bin/env python3
"""0036-R4 AC1-4 and 0036-R6 AC1-2 end-to-end acceptance.

The driver never reimplements a production algorithm.  Fixtures are written by
tests/test_large_services.c through the real YanFS API; Guest queries run the
production terminal ELF through the real executor; Host semantic queries run
the real CLI and MCP server.  The only oracles are production output and the
existing independent image/logical-identity encoders under tests/.

R4-AC2 calibration is deliberately product-driven: the candidate filler length
comes from `rebuild`/`index status` output, and the final assertions are
`postings=65536` (READY) followed by `ERROR INDEX_LIMIT` with the next word.
The same query group is then run in LIMIT state and after `index clear`, and
the command intervals are compared byte for byte.

Missing built artifacts are hard failures.  Only a missing external compiler
(`--cc`) returns 77.  Every child process and every MCP response has a
timeout.
"""

import argparse
import hashlib
import json
import os
import re
import select
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "guest"))

import run_terminal_files as base  # noqa: E402
from semantic_fixture import image as v1_image  # noqa: E402
from semantic_json_checks import logical_hash  # noqa: E402

BLOCK = 4096
MAGIC = b"YANFS01\x00"
META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {}}
GUEST_SETTLE = "2000000000"
GUEST_TIMEOUT = 900
HOST_TIMEOUT = 120


class Failure(AssertionError):
    """A named acceptance assertion failed."""


class HarnessError(RuntimeError):
    """The harness itself could not decide a named assertion."""


def require(condition, message):
    if not condition:
        raise Failure(message)


def run(argv, timeout=HOST_TIMEOUT, **kwargs):
    try:
        return subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout, **kwargs)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("timed out after %ss: %s"
                           % (timeout, " ".join(map(str, argv)))) from error
    except OSError as error:
        raise HarnessError("cannot run %s: %s" % (argv[0], error)) from error


def require_file(path, what):
    if not Path(path).is_file():
        raise Failure("missing explicit artifact %s: %s" % (what, path))


def require_inputs(source):
    inputs = [
        "tests/test_large_services.c", "tests/run_large_capacity.py",
        "tests/large_terms_probe.c", "tests/fixtures/0036-semantic-v1-reference.json",
        "tests/large_semantic_runner.py", "tests/semantic_fixture.py",
        "tests/semantic_json_checks.py", "tests/guest/run_terminal_files.py",
        "os/yanfs.c", "os/yanfs.h", "os/sort.h", "os/guest.ld",
    ]
    missing = [name for name in inputs if not (source / name).is_file()]
    if missing:
        raise Failure("missing repository input: " + ", ".join(missing))


# --------------------------------------------------------------- v2 fixture

def parse_v2(image_bytes):
    """Independent decode of a v2 image: header, CRC and every physical slot.

    This is a fixture check, not a production behavior oracle.  It lets the
    driver prove that the image really holds 1023 named files and that slot
    1022 carries the expected bytes before any query runs.
    """
    require(len(image_bytes) >= BLOCK and image_bytes[:8] == MAGIC,
            "fixture is not a YanFS image")
    version, block_size, capacity, entry_capacity = struct.unpack_from(
        "<IIII", image_bytes, 8)
    require(version == 2 and block_size == BLOCK,
            "fixture is not a v2 image")
    directory_blocks = struct.unpack_from("<I", image_bytes, 24)[0]
    require(1 <= directory_blocks <= 64, "bad directory_blocks")
    require(entry_capacity == 64 * directory_blocks - 1,
            "bad entry_capacity")
    require(len(image_bytes) == capacity * BLOCK, "bad capacity")
    directory = bytearray(image_bytes[:directory_blocks * BLOCK])
    stored = struct.unpack_from("<I", directory, 60)[0]
    directory[60:64] = b"\x00\x00\x00\x00"
    require(stored == zlib.crc32(bytes(directory)) & 0xFFFFFFFF,
            "fixture CRC mismatch")
    files = {}
    for slot in range(entry_capacity):
        if slot < 63:
            at = 64 + slot * 64
        else:
            index = slot + 1
            at = (index // 64) * BLOCK + (index % 64) * 64
        entry = directory[at:at + 64]
        if entry[0] == 0:
            require(not any(entry), "non-zero empty slot %d" % slot)
            continue
        name = entry[:32].split(b"\x00")[0].decode("ascii")
        size, start, count = struct.unpack_from("<III", entry, 32)
        data_end = start * BLOCK + size if count else 0
        require(count == (size + BLOCK - 1) // BLOCK,
                "slot %d count mismatch" % slot)
        data = image_bytes[start * BLOCK:data_end] if count else b""
        require(len(data) == size, "slot %d data truncated" % slot)
        files[slot] = (name, data)
    return files


def make_image(mkfs, fixture, path, *, filler_words=None, huge_bytes=None,
               blocks=2048):
    path = Path(path)
    path.unlink(missing_ok=True)
    result = run([mkfs, "--image", str(path), "--files", "1023",
                  "--blocks", str(blocks)], timeout=60)
    require(result.returncode == 0,
            "mkfs failed: %s" % result.stderr.decode(errors="replace"))
    argv = [fixture, "--image", str(path), "--files", "1023"]
    if filler_words is not None:
        argv += ["--filler-words", str(filler_words)]
    if huge_bytes is not None:
        argv += ["--huge-body-bytes", str(huge_bytes)]
    result = run(argv, timeout=120)
    require(result.returncode == 0,
            "fixture builder failed: %s"
            % result.stderr.decode(errors="replace"))
    return path


def verify_fixture(path):
    files = parse_v2(Path(path).read_bytes())
    require(len(files) == 1023, "fixture has %d files" % len(files))
    require(all(slot in files for slot in range(1023)),
            "fixture does not fill every slot")
    require(files[1022][0] == "slot1022.txt", "slot 1022 is not the note")
    body = files[1022][1]
    for required in (b"slotlastuniqueword", b"tag: slottag1022",
                     b"link: shared.txt", b"@block zslot",
                     b"[[shared.txt#anchor]]"):
        require(required in body, "slot 1022 lacks %r" % required)
    require(files[1000][0] == "a.txt" and b"@block x" in files[1000][1],
            "a.txt is not at slot 1000")
    return files


# ------------------------------------------------------------------- Guest

def guest(args, work, image, commands, label):
    stdin = b"\n".join(commands + [b"exit"]) + b"\n"
    result = base.run_guest(args.run, args.guest, image, stdin, work, label,
                            max_steps=GUEST_SETTLE)
    base.check_not_harness(result, label)
    base.require_rc(result, 0, label)
    return result


def interval(result, command, label, cursor=0):
    echo = b"yanfs> " + command + b"\r\n"
    at = result.stdout.find(echo, cursor)
    if at < 0:
        raise Failure("%s: missing echoed %r" % (label, command))
    start = at + len(echo)
    end = result.stdout.find(b"yanfs> ", start)
    if end < 0:
        raise Failure("%s: no prompt after %r" % (label, command))
    return result.stdout[start:end], end


def index_status(result, label, cursor=0):
    text, end = interval(result, b"index status", label, cursor)
    match = re.search(rb"INDEX state=(\w+) source=(\w+) terms=(\d+) "
                      rb"postings=(\d+)\r\nOK index\r\n", text)
    if not match:
        raise Failure("%s: malformed index status %r" % (label, text))
    state, source, terms, postings = (match.group(1).decode(),
                                      match.group(2).decode(),
                                      int(match.group(3)), int(match.group(4)))
    return state, source, terms, postings, end


def status_postings(args, work, image, label):
    result = guest(args, work, image, [b"rebuild", b"index status"], label)
    rebuild, cursor = interval(result, b"rebuild", label)
    state, _source, _terms, postings, _ = index_status(result, label, cursor)
    return rebuild, state, postings, result


# --------------------------------------------------------------------- MCP

class Wire:
    """One real yan_mcp stdio process; every reply has a wall-clock bound."""

    def __init__(self, server, image, index, model, runner):
        self.argv = [str(server), "--disk-image", str(image),
                     "--index", str(index), "--model", str(model),
                     "--runner", str(runner), "--startup-timeout", "10",
                     "--request-timeout", "30"]
        self.process = subprocess.Popen(
            self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self.buffer = b""

    def _read_line(self, timeout=60):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0:
                raise HarnessError("MCP response timed out")
            if not select.select([self.process.stdout], [], [], left)[0]:
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise HarnessError("MCP server exited before a reply: %r"
                                   % self.process.stderr.read())
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line

    def call(self, tool, arguments=None, ident=1, timeout=60):
        request = {"jsonrpc": "2.0", "id": ident, "method": "tools/call",
                   "params": {"_meta": META, "name": tool,
                              "arguments": arguments or {}}}
        self.process.stdin.write(
            json.dumps(request, separators=(",", ":")).encode() + b"\n")
        self.process.stdin.flush()
        line = self._read_line(timeout)
        reply = json.loads(line)
        if "error" in reply:
            raise Failure("MCP protocol error for %s: %r" % (tool, reply))
        result = reply["result"]
        require(result.get("resultType") == "complete",
                "MCP reply is not complete")
        if result.get("isError"):
            text = json.loads(result["content"][0]["text"])
            require(set(text) == {"code"}, "MCP error is not a code object")
            return text["code"], None
        require(isinstance(result.get("structuredContent"), dict),
                "MCP success lacks structuredContent")
        require("content" in result and len(result["content"]) == 1,
                "MCP success lacks one content item")
        require(json.loads(result["content"][0]["text"])
                == result["structuredContent"],
                "MCP text and structuredContent differ")
        return None, result["structuredContent"]

    def close(self, expected=0):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise HarnessError("MCP server did not exit after EOF")
        stderr = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        if b"Sanitizer" in stderr or b"runtime error:" in stderr:
            raise HarnessError("MCP sanitizer output: %r" % stderr)
        require(self.process.returncode == expected,
                "MCP exit %d, expected %d: %r"
                % (self.process.returncode, expected, stderr))


# ----------------------------------------------------------------- semantic

def semantic_cli(tool, command, image, index, model, runner, extra=(),
                 timeout=HOST_TIMEOUT):
    argv = [str(tool), command, "--disk-image", str(image),
            "--index", str(index), "--model", str(model),
            "--runner", str(runner), *extra]
    return run(argv, timeout=timeout)


def semantic_json(tool, command, image, index, model, runner, extra=()):
    result = semantic_cli(tool, command, image, index, model, runner, extra)
    require(result.returncode == 0,
            "%s %s failed rc=%d stderr=%r stdout=%r"
            % (tool, command, result.returncode, result.stderr,
               result.stdout[:200]))
    return json.loads(result.stdout)


def write_model(path):
    path.write_text("universal")
    return path


# --------------------------------------------------------------------- cases

def case_r4_ac1(args, work, fixture, image):
    files = verify_fixture(image)
    expected = logical_hash([files.get(slot) and
                             (files[slot][0], files[slot][1])
                             for slot in range(1023)])
    commands = [
        b"search slotlastuniqueword",
        b"search tag:slottag1022 slotlastuniqueword",
        b"links slot1022.txt",
        b"backlinks shared.txt",
        b"related relsrc.txt",
        b"block refs slot1022.txt",
        b"index status",
    ]
    result = guest(args, work, image, commands, "r4-ac1")
    cursor = 0

    text, cursor = interval(result, commands[0], "r4-ac1", cursor)
    require(b"slot1022.txt:" in text and b"slotlastuniqueword" in text,
            "R4-AC1 search did not hit slot 1022")
    require(b"OK search total=1 shown=1 skipped=0 mode=index" in text,
            "R4-AC1 search summary is not the unique indexed row")

    text, cursor = interval(result, commands[1], "r4-ac1", cursor)
    require(b"slot1022.txt:" in text and b"OK search total=1 shown=1" in text,
            "R4-AC1 tag search did not hit slot 1022")

    text, cursor = interval(result, commands[2], "r4-ac1", cursor)
    require(b"LINK shared.txt OK\r\n" in text,
            "R4-AC1 links did not show slot 1022's link")

    text, cursor = interval(result, commands[3], "r4-ac1", cursor)
    require(b"BACKLINK slot1022.txt\r\n" in text,
            "R4-AC1 backlinks did not show slot 1022")

    text, cursor = interval(result, commands[4], "r4-ac1", cursor)
    require(b"RELATED slot1022.txt" in text,
            "R4-AC1 related did not show slot 1022")

    text, cursor = interval(result, commands[5], "r4-ac1", cursor)
    require(b"target=shared.txt#anchor state=OK" in text,
            "R4-AC1 block refs did not resolve slot 1022's reference")
    require(b"block=#zslot" in text,
            "R4-AC1 block refs lost the source block id")

    state, source, _terms, postings, _ = index_status(result, "r4-ac1", cursor)
    require(state == "READY" and source == "MOUNTED",
            "R4-AC1 index is not READY/MOUNTED")
    require(postings > 0, "R4-AC1 index reports no postings")

    index = work / "r4-ac1.index"
    model = write_model(work / "model")
    runner = str(args.semantic_runner)
    wire = Wire(args.mcp, image, index, model, runner)
    try:
        code, value = wire.call(
            "yanos_block_read",
            {"name": "slot1022.txt", "block_id": "zslot"})
    finally:
        wire.close()
    require(code is None, "R4-AC1 MCP block_read failed: %r" % code)
    require(value["operation"] == "block_read", "MCP operation differs")
    require(value["name"] == "slot1022.txt", "MCP name differs")
    require(value["block_id"] == "zslot", "MCP block id differs")
    require("[[shared.txt#anchor]]" in value["text"],
            "MCP block_read text does not contain the reference")
    require(value["source_sha256"] == expected,
            "MCP source_sha256 is not the independent logical identity")
    return {"postings": postings}


def case_r4_ac3(args, work, image):
    commands = [b"block backlinks a.txt#x", b"backlinks a.txt"]
    expected = [b"n%04d.txt" % i for i in range(20)]
    result = guest(args, work, image, commands, "r4-ac3")
    text, _ = interval(result, commands[0], "r4-ac3")
    rows = [line for line in text.split(b"\r\n") if line.startswith(b"BACKLINK ")]
    require(len(rows) == 20, "block backlinks displayed %d rows" % len(rows))
    block_names = [row.split(b"source=", 1)[-1].split(b" ", 1)[0]
                   for row in rows]
    require(block_names == expected,
            "block backlinks are not the ASCII-first 20: %r" % block_names)
    require(b"OK block backlinks total=1000 shown=20 skipped=0 target=OK" in text,
            "block backlinks total/shown differs: %r" % text[-120:])
    text, _ = interval(result, commands[1], "r4-ac3")
    rows = [line for line in text.split(b"\r\n") if line.startswith(b"BACKLINK ")]
    require(len(rows) == 20, "backlinks displayed %d rows" % len(rows))
    file_names = [row[len(b"BACKLINK "):] for row in rows]
    require(file_names == expected,
            "backlinks are not the ASCII-first 20: %r" % file_names)
    match = re.search(rb"OK backlinks(?: total=(\d+))? shown=(\d+) "
                      rb"target=OK\r\n", text)
    require(match is not None, "backlinks receipt is malformed: %r"
            % text[-120:])
    require(match.group(1) == b"1000",
            "backlinks total is not 1000: %r" % text[-120:])
    require(match.group(2) == b"20",
            "backlinks shown is not 20: %r" % text[-120:])
    return {"total": 1000, "shown": 20,
            "file_names": [name.decode("ascii") for name in file_names],
            "block_names": [name.decode("ascii") for name in block_names]}


def case_r4_ac2(args, work, fixture, ac1_image):
    # Product output, not a test tokenizer, supplies the real baseline count.
    _rebuild, state, baseline_postings, _ = status_postings(
        args, work, ac1_image, "r4-ac2-baseline")
    require(state == "READY", "baseline image index is not READY")
    words = 65536 - baseline_postings
    boundary = work / "r4-ac2-boundary.img"
    attempts = []
    for attempt in range(6):
        make_image(args.mkfs, fixture, boundary, filler_words=words)
        verify_fixture(boundary)
        rebuild, state, postings, _ = status_postings(
            args, work, boundary, "r4-ac2-calibrate-%d" % attempt)
        attempts.append({"filler_words": words, "state": state,
                         "postings": postings})
        if state == "READY":
            if rebuild != b"OK rebuild\r\n":
                raise Failure("boundary rebuild failed: %r" % rebuild)
            if postings == 65536:
                break
            words += 65536 - postings
        elif state == "LIMIT":
            words = max(0, words - 1024)
        else:
            raise Failure("unexpected boundary state %r" % state)
    else:
        raise Failure("could not calibrate postings=65536: %r" % attempts)
    require(attempts[-1]["postings"] == 65536,
            "product did not report postings=65536")

    limit = work / "r4-ac2-limit.img"
    make_image(args.mkfs, fixture, limit, filler_words=words + 1)
    verify_fixture(limit)

    queries = [
        b"search slotlastuniqueword",
        b"search tag:slottag1022 slotlastuniqueword",
        b"links slot1022.txt",
        b"backlinks shared.txt",
        b"related relsrc.txt",
        b"block refs slot1022.txt",
        b"block backlinks a.txt#x",
    ]
    result = guest(args, work, limit,
                   [b"rebuild", b"index status", *queries], "r4-ac2-limit")
    rebuild, cursor = interval(result, b"rebuild", "r4-ac2-limit")
    require(rebuild == b"ERROR INDEX_LIMIT\r\n",
            "one more posting did not fail the rebuild: %r" % rebuild)
    state, _source, _terms, postings, cursor = index_status(
        result, "r4-ac2-limit", cursor)
    require(state == "LIMIT" and postings == 0,
            "limit image status is %r/%d" % (state, postings))
    first = {}
    for command in queries:
        text, cursor = interval(result, command, "r4-ac2-limit", cursor)
        first[command] = text
    require(b"mode=scan" in first[queries[0]],
            "LIMIT search did not use the scan fallback")
    require(b"slot1022.txt:" in first[queries[0]],
            "LIMIT search lost slot 1022")

    result = guest(args, work, limit,
                   [b"rebuild", b"index clear", b"index status", *queries],
                   "r4-ac2-scan")
    cursor = 0
    _, cursor = interval(result, b"rebuild", "r4-ac2-scan")
    clear, cursor = interval(result, b"index clear", "r4-ac2-scan")
    require(clear == b"OK index\r\n", "index clear failed")
    state, _source, _terms, postings, cursor = index_status(
        result, "r4-ac2-scan", cursor)
    require(state == "EMPTY" and postings == 0,
            "cleared index status is %r/%d" % (state, postings))
    second = {}
    for command in queries:
        text, cursor = interval(result, command, "r4-ac2-scan", cursor)
        second[command] = text
    for command in queries:
        require(first[command] == second[command],
                "R4-AC2 %r differs between LIMIT and forced SCAN\n"
                "limit=%r\nscan=%r"
                % (command, first[command], second[command]))
    require(b"mode=scan" in second[queries[0]],
            "forced SCAN did not report mode=scan")
    score_queries = ("slotlastuniqueword", "w000", "w000 w001")
    for query in score_queries:
        outputs = []
        for mode in ("limit", "scan"):
            probe = run([str(args.terms_probe), str(limit), mode, query])
            require(probe.returncode == 0, "score probe failed: %r" % probe.stderr)
            outputs.append(probe.stdout)
        require(outputs[0] == outputs[1], "scores/order/snippets differ for %r" % query)
        matches = [line.split() for line in outputs[0].splitlines() if line.startswith(b"MATCH ")]
        require(matches and all(int(row[3]) > 0 for row in matches), "score probe returned no scored rows")
        if query == "slotlastuniqueword":
            require(matches[0][1] == b"slot1022.txt" and matches[0][3] == b"1", "unique-word score is not 1")
    return {"baseline_postings": baseline_postings,
            "calibration": attempts,
            "limit_postings": 65537,
            "queries_compared": len(queries), "score_queries_compared": len(score_queries)}


def case_r4_ac4(args, work):
    files = [("a.txt", b"@yan-note 1\n@end\n@block first\nhello identity\n"),
             ("b.txt", b"plain bytes\n"),
             ("binary", b"\x00\xff")]
    image = work / "r4-ac4-v1.img"
    image.write_bytes(v1_image(files))
    digest = logical_hash(files)
    index = work / "r4-ac4.index"
    model = write_model(work / "model")
    runner = str(args.semantic_runner)

    values = {}
    servers = [("new", args.mcp)]
    if args.baseline_mcp is not None:
        servers.append(("baseline", args.baseline_mcp))
    for label, server in servers:
        wire = Wire(server, image, index, model, runner)
        try:
            code, value = wire.call("yanos_block_list", {"name": "a.txt"})
        finally:
            wire.close()
        require(code is None, "%s MCP block_list failed: %r" % (label, code))
        values[label] = value["source_sha256"]
    reference = json.loads((args.source / "tests/fixtures/0036-semantic-v1-reference.json").read_text())
    require(all(value == reference["r4_source_sha256"] for value in values.values()),
            "R4-AC4 MCP source_sha256 differs from captured 41b83b5 identity")
    require(values["new"] == digest,
            "R4-AC4 MCP source_sha256 is not the independent identity")

    cli = {}
    tools = [("new", args.semantic)]
    if args.baseline_semantic is not None:
        tools.append(("baseline", args.baseline_semantic))
    for label, tool in tools:
        cli_index = work / ("r4-ac4-%s.index" % label)
        cli_index.unlink(missing_ok=True)
        result = semantic_cli(tool, "rebuild", image, cli_index, model, runner)
        require(result.returncode == 0, "%s v1 rebuild failed: %r"
                % (label, result.stderr))
        value = semantic_json(tool, "search", image, cli_index, model, runner,
                              ("--query", "hello", "--format", "json"))
        cli[label] = value["source_sha256"]
    require(all(value == digest for value in cli.values()),
            "R4-AC4 CLI source_sha256 differs: %r" % cli)
    return {"source_sha256": digest}


def case_r6_ac1(args, work, image, index):
    files = verify_fixture(image)
    digest = logical_hash([files.get(slot) and
                           (files[slot][0], files[slot][1])
                           for slot in range(1023)])
    model = write_model(work / "model")
    runner = str(args.semantic_runner)
    index.unlink(missing_ok=True)

    result = semantic_cli(args.semantic, "rebuild", image, index, model, runner)
    require(result.returncode == 0, "R6-AC1 rebuild failed: %r" % result.stderr)
    require(re.fullmatch(rb"OK rebuild segments=\d+ skipped=0\n",
                         result.stdout) is not None,
            "R6-AC1 rebuild output differs: %r" % result.stdout)

    value = semantic_json(args.semantic, "search", image, index, model, runner,
                          ("--query", "slotlastuniqueword",
                           "--format", "json"))
    require(value["source_sha256"] == digest,
            "R6-AC1 semantic source_sha256 differs")
    require(value.get("results") and
            value["results"][0]["file"] == "slot1022.txt",
            "R6-AC1 semantic search did not rank slot 1022 first: %r"
            % value.get("results"))
    require("slotlastuniqueword" in value["results"][0]["text"],
            "R6-AC1 semantic row text differs")

    status = semantic_cli(args.semantic, "status", image, index, model, runner)
    require(status.returncode == 0, "R6-AC1 status failed: %r" % status.stderr)
    match = re.fullmatch(rb"STATUS READY segments=\d+ skipped=0\n",
                         status.stdout)
    require(match is not None, "R6-AC1 status output differs: %r"
            % status.stdout)

    wire = Wire(args.mcp, image, index, model, runner)
    try:
        code, value = wire.call("yanos_index_status")
        require(code is None and value["state"] == "READY",
                "R6-AC1 MCP index status is not READY: %r" % (code or value))
        code, value = wire.call("yanos_search",
                                {"query": "slotlastuniqueword"})
    finally:
        wire.close()
    require(code is None, "R6-AC1 MCP search failed: %r" % code)
    require(value["source_sha256"] == digest,
            "R6-AC1 MCP search source_sha256 differs")
    require(value["results"] and value["results"][0]["file"] == "slot1022.txt",
            "R6-AC1 MCP search did not rank slot 1022 first")

    # R6-AC1 v1 part: byte-identical index against the 41b83b5 build.
    v1_files = [("a.txt", b"@yan-note 1\n@end\nfirst v1 body\n"),
                ("b.txt", b"@yan-note 1\ntitle: second\n@end\nsecond body\n")]
    v1 = work / "r6-ac1-v1.img"
    v1.write_bytes(v1_image(v1_files))
    new_index = work / "r6-ac1-v1-new.index"
    old_index = work / "r6-ac1-v1-base.index"
    new_index.unlink(missing_ok=True)
    old_index.unlink(missing_ok=True)
    new = semantic_cli(args.semantic, "rebuild", v1, new_index, model, runner)
    require(new.returncode == 0, "R6-AC1 v1 rebuild failed: %r" % new.stderr)
    reference = json.loads((args.source / "tests/fixtures/0036-semantic-v1-reference.json").read_text())
    normalized = bytearray(new_index.read_bytes())
    require(len(normalized) == reference["index_bytes"], "v1 index length changed")
    require(normalized[32:64].hex() == reference["r6_source_sha256"] == logical_hash(v1_files), "v1 source identity changed")
    # Only identities of external dependencies vary with fixture runner edits;
    # the captured source, counts, payload checksum and record bytes stay exact.
    normalized[64:160] = bytes(96)
    require(hashlib.sha256(normalized).hexdigest() == reference["identity_independent_index_sha256"],
            "v1 format or payload differs from captured 41b83b5 output")
    if args.baseline_semantic is not None:
        old = semantic_cli(args.baseline_semantic, "rebuild", v1, old_index, model, runner)
        require(old.returncode == 0, "baseline v1 rebuild failed: %r" % old.stderr)
        require(new_index.read_bytes() == old_index.read_bytes(), "v1 index bytes differ from 41b83b5 build")
    return {"v1_index_sha256": hashlib.sha256(
        new_index.read_bytes()).hexdigest()}


def case_r6_ac2(args, work, fixture):
    model = write_model(work / "model")
    runner = str(args.semantic_runner)
    # Pure segment cap on a real v1 image.  A 1000-byte body publishes an old
    # index; replacing the same image with a 4,000,000-byte body must not
    # publish a partial index or touch the old bytes.
    small = work / "r6-ac2-v1.img"
    small.write_bytes(v1_image([("huge.txt",
                                 b"@yan-note 1\n@end\n" + b"a" * 1000 + b"\n")]))
    index = work / "r6-ac2-v1.index"
    index.unlink(missing_ok=True)
    rebuilt = semantic_cli(args.semantic, "rebuild", small, index, model, runner)
    require(rebuilt.returncode == 0, "R6-AC2 v1 seed rebuild failed: %r"
            % rebuilt.stderr)
    old_bytes = index.read_bytes()
    small.write_bytes(v1_image([("huge.txt",
                                 b"@yan-note 1\n@end\n" + b"a" * 4000000 + b"\n")]))
    for command in ("rebuild", "search", "status"):
        extra = ("--query", "a") if command == "search" else ()
        result = semantic_cli(args.semantic, command, small, index, model,
                              runner, extra)
        require(result.returncode == 3,
                "R6-AC2 v1 %s did not exit 3: rc=%d stderr=%r"
                % (command, result.returncode, result.stderr))
        require(result.stderr == b"ERROR LIMIT\n" and result.stdout == b"",
                "R6-AC2 v1 %s did not fail atomically: %r/%r"
                % (command, result.stdout, result.stderr))
        require(index.read_bytes() == old_bytes,
                "R6-AC2 v1 %s changed the old index" % command)
    index.unlink()
    result = semantic_cli(args.semantic, "rebuild", small, index, model, runner)
    require(result.returncode == 3 and result.stderr == b"ERROR LIMIT\n",
            "R6-AC2 v1 fresh LIMIT differs")
    require(not index.exists(), "R6-AC2 v1 published a partial index")

    # High-slot combined case: N=1023 image with a >8192-segment note.
    ac1 = make_image(args.mkfs, fixture, work / "r6-ac2-ac1.img")
    verify_fixture(ac1)
    old_index = work / "r6-ac2-v2.index"
    old_index.unlink(missing_ok=True)
    rebuilt = semantic_cli(args.semantic, "rebuild", ac1, old_index, model,
                           runner)
    require(rebuilt.returncode == 0, "R6-AC2 high-slot seed rebuild failed: %r"
            % rebuilt.stderr)
    old_bytes = old_index.read_bytes()
    huge = make_image(args.mkfs, fixture, work / "r6-ac2-v2-huge.img",
                      huge_bytes=4000000)
    verify_fixture(huge)
    require(huge.read_bytes() != ac1.read_bytes(),
            "R6-AC2 huge fixture did not change the image")
    for command in ("rebuild", "search", "status"):
        extra = ("--query", "a") if command == "search" else ()
        result = semantic_cli(args.semantic, command, huge, old_index, model,
                              runner, extra)
        require(result.returncode == 3 and result.stderr == b"ERROR LIMIT\n"
                and result.stdout == b"",
                "R6-AC2 high-slot %s did not fail with LIMIT only: rc=%d "
                "stdout=%r stderr=%r"
                % (command, result.returncode, result.stdout, result.stderr))
        require(old_index.read_bytes() == old_bytes,
                "R6-AC2 high-slot %s changed the old index" % command)

    wire = Wire(args.mcp, huge, old_index, model, runner)
    try:
        code, value = wire.call("yanos_search", {"query": "a"})
        require(code == "LIMIT", "R6-AC2 MCP search code is %r" % code)
        code, value = wire.call("yanos_index_status")
        require(code == "LIMIT", "R6-AC2 MCP status code is %r" % code)
        code, value = wire.call("yanos_block_read",
                                {"name": "slot1022.txt",
                                 "block_id": "zslot"})
    finally:
        wire.close()
    require(code is None,
            "R6-AC2 MCP block_read failed at high slot: %r" % code)
    require("[[shared.txt#anchor]]" in value["text"],
            "R6-AC2 MCP block_read text differs")
    require(old_index.read_bytes() == old_bytes,
            "R6-AC2 MCP calls changed the old index")
    return {"v1_index_sha256": hashlib.sha256(old_bytes).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--mkfs", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    parser.add_argument("--semantic", required=True, type=Path)
    parser.add_argument("--mcp", required=True, type=Path)
    parser.add_argument("--semantic-runner", required=True, type=Path)
    parser.add_argument("--terms-probe", required=True, type=Path)
    parser.add_argument("--baseline-semantic", type=Path)
    parser.add_argument("--baseline-mcp", type=Path)
    args = parser.parse_args()
    args.source = args.source.resolve()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)

    require_inputs(args.source)
    require((args.baseline_semantic is None) == (args.baseline_mcp is None), "both baseline paths must be supplied together")
    for name, path in (("mkfs", args.mkfs), ("yan_run", args.run),
                       ("Guest ELF", args.guest),
                       ("yan_semantic", args.semantic),
                       ("yan_mcp", args.mcp),
                       ("semantic runner", args.semantic_runner),
                       ("terms score probe", args.terms_probe)):
        require_file(path, name)
    if args.baseline_semantic is not None:
        require_file(args.baseline_semantic, "41b83b5 yan_semantic")
        require_file(args.baseline_mcp, "41b83b5 yan_mcp")
    if not (Path(args.cc).is_file() or shutil.which(args.cc)):
        print("SKIP large-capacity: host compiler unavailable")
        return 77
    fixture = args.work / "large_fixture"
    build = run([args.cc, "-std=c17", "-O2", "-Wall", "-Wextra",
                 "-Wpedantic", "-Werror", "-I", str(args.source / "os"),
                 str(args.source / "tests/test_large_services.c"),
                 str(args.source / "os/yanfs.c"), "-o", str(fixture)],
                timeout=120)
    require(build.returncode == 0,
            "fixture builder build failed: %s"
            % build.stderr.decode(errors="replace"))

    image = make_image(args.mkfs, fixture, args.work / "r4-ac1.img")
    evidence = {}


    def record(name, function):
        try:
            evidence[name] = function()
            print("PASS", name)
        except (Failure, AssertionError) as error:
            evidence[name] = {"failure": str(error)}
            print("FAIL", name + ":", error)


    record("r4-ac1", lambda: case_r4_ac1(args, args.work, fixture, image))
    record("r4-ac2", lambda: case_r4_ac2(args, args.work, fixture, image))
    record("r4-ac3", lambda: case_r4_ac3(args, args.work, image))
    record("r4-ac4", lambda: case_r4_ac4(args, args.work))
    record("r6-ac1", lambda: case_r6_ac1(args, args.work, image,
                                         args.work / "r6-ac1.index"))
    record("r6-ac2", lambda: case_r6_ac2(args, args.work, fixture))

    (args.work / "large-capacity-evidence.json").write_text(
        json.dumps(evidence, indent=2) + "\n")
    failed = [name for name in ("r4-ac1", "r4-ac2", "r4-ac3", "r4-ac4",
                                "r6-ac1", "r6-ac2")
              if "failure" in evidence.get(name, {"failure": "missing"})]
    if failed:
        print("FAIL large-capacity cases:", ", ".join(failed))
        return 1
    print("PASS 0036-R4 AC1-4 / R6 AC1-2 large capacity")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as error:
        print("FAIL large-capacity:", error)
        sys.exit(1)
    except (HarnessError, OSError, ValueError, KeyError,
            subprocess.SubprocessError) as error:
        print("HARNESS large-capacity:", error)
        sys.exit(2)
