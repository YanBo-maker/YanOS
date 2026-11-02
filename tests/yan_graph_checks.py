#!/usr/bin/env python3
"""0044 R2 acceptance: the real `yan_graph` over real YanFS images.

Every expected value is computed here from the fixture text that this file
wrote, by an independent raw-byte reader, and compared against JSON parsed with
`json.loads`. Nothing is compared by grepping the CLI's stdout:

  * block and declaration byte ranges and physical lines come from a scan of the
    fixture bytes done by this module (`_scan`), not from the CLI;
  * `source_sha256` is recomputed from the image with the 0033 identity recipe
    (sorted ASCII names; slot, name length, name, size, then every raw byte);
  * the image bytes are read before and after every invocation to prove the tool
    performs zero writes.

A missing in-repo input or the real binary is a hard failure (exit 1). Only a
missing external dependency may skip (77), and this file needs none. A case
failure prints ":FAIL: <owner>: ..." and returns 1; a harness problem (crash,
timeout) is exit 2.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

BLOCK = 4096
MAGIC = b"YANFS01\x00"
TIMEOUT = 120


class HarnessError(RuntimeError):
    """Not a clean pass or fail of an assertion: a broken harness."""


# --------------------------------------------------------------- image writer

def _reseal(raw, directory_blocks):
    """Recompute the directory CRC in place: offsets 60..63 are its own slot."""
    crc = zlib.crc32(raw[:60] + b"\x00\x00\x00\x00" + raw[64:directory_blocks * BLOCK])
    struct.pack_into("<I", raw, 60, crc & 0xFFFFFFFF)


def build_image(files, capacity=63, blocks=None):
    """A valid YanFS image holding `files`, written independently of the tools.

    `files` is [(name, bytes)] in slot order; names are ASCII and the data is
    placed in consecutive extents after the directory, exactly as 0021/0036
    describe. The CRC is computed here, so the CLI's mount is a real check of
    this encoder rather than a shared assumption.
    """
    directory_blocks = (capacity + 1) // 64
    needed = directory_blocks + sum(
        max(1, (len(data) + BLOCK - 1) // BLOCK) if data else 0
        for _, data in files)
    if blocks is None:
        blocks = max(needed, directory_blocks + 1)
    if blocks < needed:
        raise HarnessError("fixture does not fit its image")
    raw = bytearray(blocks * BLOCK)
    raw[0:8] = MAGIC
    struct.pack_into("<I", raw, 8, 1 if directory_blocks == 1 else 2)
    struct.pack_into("<I", raw, 12, BLOCK)
    struct.pack_into("<I", raw, 16, blocks)
    struct.pack_into("<I", raw, 20, capacity)
    if directory_blocks != 1:
        struct.pack_into("<I", raw, 24, directory_blocks)
    lba = directory_blocks
    for slot, (name, data) in enumerate(files):
        at = 64 + slot * 64
        encoded = name.encode("ascii")
        raw[at:at + len(encoded)] = encoded
        count = (len(data) + BLOCK - 1) // BLOCK if data else 0
        struct.pack_into("<III", raw, at + 32, len(data),
                         lba if count else 0, count)
        if data:
            raw[lba * BLOCK:lba * BLOCK + len(data)] = data
        lba += count
    _reseal(raw, directory_blocks)
    return bytes(raw)


def source_digest(image):
    """0033 logical source identity, recomputed here from the image bytes.

    Sorted ASCII file names; then per file slot u32LE, name length u32LE, name
    bytes, size u32LE and every raw byte. Binary files contribute unchanged.
    """
    capacity = struct.unpack_from("<I", image, 20)[0]
    files = []
    for slot in range(capacity):
        entry = image[64 + slot * 64:128 + slot * 64]
        if entry[0] == 0:
            continue
        name = entry[:32].split(b"\x00", 1)[0]
        size, start, _count = struct.unpack_from("<III", entry, 32)
        files.append((slot, name, size, start))
    files.sort(key=lambda item: item[1])
    hasher = hashlib.sha256()
    for slot, name, size, start in files:
        hasher.update(struct.pack("<I", slot))
        hasher.update(struct.pack("<I", len(name)))
        hasher.update(name)
        hasher.update(struct.pack("<I", size))
        hasher.update(image[start * BLOCK:start * BLOCK + size])
    return hasher.hexdigest()


# ------------------------------------------------------- independent raw oracle

def _lines(body, body_offset, first_line):
    """Every physical line: (line number, start, content len, offset after EOL).

    Content excludes the CR/LF terminator, exactly as the specification defines
    the declaration range; the fourth value is the byte just past the terminator,
    which is where a following block's content begins. A final line without a
    newline is still a line and ends at EOF.
    """
    result = []
    line = first_line
    start = body_offset
    index = 0
    limit = body_offset + len(body)
    while index <= len(body):
        if index == len(body):
            if start < limit:
                result.append((line, start, limit - start, limit))
            break
        if body[index:index + 1] == b"\n":
            end = body_offset + index
            length = end - start
            if length and body[index - 1:index] == b"\r":
                length -= 1
            result.append((line, start, length, end + 1))
            line += 1
            start = end + 1
        index += 1
    return result


def parse_notes(files):
    """The graph a correct reader derives from `files`, computed from the bytes.

    This is the oracle: it walks the same documented grammar using its own code,
    so it does not import the implementation.
    """
    concepts = {}    # id -> {"name", "aliases", "declarations"}
    relations = []   # dicts in canonical order
    skipped = 0
    for name, data in files:
        text_ok = _text_eligible(data)
        if not text_ok:
            skipped += 1
            continue
        header_bytes, first_body_line = _header(data)
        blocks = _blocks(data, header_bytes, first_body_line)
        for line_number, start, length, _after in _lines(data[header_bytes:],
                                                         header_bytes,
                                                         first_body_line):
            content = data[start:start + length]
            if not content.startswith(b"@kg"):
                continue
            block = _block_at(blocks, start)
            if block is None:
                raise AssertionError("fixture has a directive outside a block")
            _apply(concepts, relations, name, block, content, line_number,
                   start, length)
    ordered = []
    for cid in sorted(concepts):
        entry = concepts[cid]
        entry["aliases"].sort()
        ordered.append(entry)
    relations.sort(key=lambda r: (r["file"], r["declaration_start"],
                                  r["type"], r["subject"], r["object"]))
    return ordered, relations, skipped


def _text_eligible(data):
    try:
        text = data.decode("utf-8", "strict")
    except UnicodeDecodeError:
        return False
    for position, character in enumerate(text):
        code = ord(character)
        if code == 9 or code == 10:
            continue
        if code == 0x0D:
            # A CR is legal only as the first half of CRLF; it is a C0 control
            # otherwise, so this rule has to be decided before the range check.
            if position + 1 >= len(text) or text[position + 1] != "\n":
                return False
            continue
        if code < 0x20 or code == 0x7F or 0x80 <= code <= 0x9F:
            return False
    return True


def _header(data):
    """(body offset, body first physical line) from the 0027 header, if any."""
    if not data.startswith(b"@yan-note"):
        return 0, 1
    end = data.find(b"\n@end\n")
    if end < 0:
        raise AssertionError("fixture header is not closed")
    body = end + len(b"\n@end\n")
    return body, data[:body].count(b"\n") + 1


def _blocks(data, body_offset, first_line):
    """Block content extents, from the fixture bytes alone.

    A block's content starts after its marker line and ends at the byte offset of
    the next marker line, or at EOF. `first`/`last` are the physical lines of
    that content; an empty block reports 0 for both, as the specification says.
    """
    markers = []
    for line_number, start, length, after in _lines(data[body_offset:],
                                                    body_offset, first_line):
        content = data[start:start + length]
        if content.startswith(b"@block"):
            markers.append((line_number, start, after,
                            content[len(b"@block "):].decode("ascii")))
    blocks = []
    for index, (line_number, start, content_start, bid) in enumerate(markers):
        if index + 1 < len(markers):
            content_end = markers[index + 1][1]
        else:
            content_end = len(data)
        if content_start == content_end:
            blocks.append((bid, content_start, content_end, line_number, 0, 0))
            continue
        first = line_number + 1
        # A trailing LF ends the line it terminates, so the block's last physical
        # line is one less than the newline count in that case; a final line
        # without a newline still belongs to the block.
        tail = data[content_start:content_end]
        last = first + tail.count(b"\n")
        if tail.endswith(b"\n"):
            last = max(first, last - 1)
        blocks.append((bid, content_start, content_end, line_number, first,
                       last))
    return blocks


def _block_at(blocks, offset):
    for block in blocks:
        if block[1] <= offset < block[2]:
            return block
    return None


def _apply(concepts, relations, name, block, content, line_number, start, length):
    """One reserved line. The command word is field 0's tail; fields follow."""
    parts = content.split(b" ")
    if parts[0] != b"@kg":
        raise AssertionError("fixture line is not reserved: %r" % content)
    command = parts[1]
    when = {
        "file": name,
        "block_id": block[0],
        "declaration_line": line_number,
        "declaration_start": start,
        "declaration_end": start + length,
        "block_marker_line": block[3],
        "block_first_line": block[4],
        "block_last_line": block[5],
        "block_start": block[1],
        "block_end": block[2],
    }
    if command == b"concept":
        cid = parts[2].decode("ascii")
        label = b" ".join(parts[3:]).decode("utf-8")
        entry = concepts.setdefault(cid, {"id": cid, "name": label,
                                          "aliases": [], "declarations": []})
        entry["declarations"].append(when)
    elif command == b"alias":
        cid = parts[2].decode("ascii")
        label = b" ".join(parts[3:]).decode("utf-8")
        concepts[cid]["aliases"].append(label)
    elif command == b"relation":
        relations.append({
            "type": parts[2].decode("ascii"),
            "subject": parts[3].decode("ascii"),
            "object": parts[4].decode("ascii"),
            **when,
        })
    else:
        raise AssertionError("fixture uses an unknown command %r" % command)


# ----------------------------------------------------------------- CLI driving

class Runner:
    def __init__(self, tool, fault=None):
        self.tool = str(tool)
        self.fault = None if fault is None else str(fault)
        self.work = Path(tempfile.mkdtemp(prefix="yan-graph-"))
        self.cases = 0

    def image(self, name, files, **kwargs):
        path = self.work / (name + ".img")
        data = build_image(files, **kwargs)
        path.write_bytes(data)
        return path, data

    def run_fault(self, owner, mode, exit_code, token, *arguments, image=None,
                  expect_image_change=False):
        """One invocation of the test-only fault probe, which wraps the real CLI.

        The probe is a separate linked binary, so the production tool keeps no
        fault switch; only the wrapped call is altered.
        """
        if self.fault is None:
            raise AssertionError("%s: no fault probe was provided" % owner)
        before = None
        if arguments[0] == "--disk-image":
            before = Path(arguments[1]).read_bytes()
        environment = {**os.environ, "YAN_KG_FAULT": mode,
                       "PYTHONDONTWRITEBYTECODE": "1"}
        if image is not None:
            environment["YAN_KG_FAULT_IMAGE"] = str(image)
        result = subprocess.run([self.fault, *map(str, arguments)],
                                capture_output=True, timeout=TIMEOUT,
                                env=environment)
        # The production tool must never write. The `rewrite` mode deliberately
        # makes the TEST INJECTOR change its own throw-away fixture, so only that
        # mode may be non-identical, and the caller checks what changed.
        identical = before is None or Path(arguments[1]).read_bytes() == before
        if expect_image_change:
            if identical:
                raise AssertionError("%s: the injector made no change" % owner)
        elif not identical:
            raise AssertionError("%s: the image changed" % owner)
        if result.returncode != exit_code:
            raise AssertionError("%s: exit %d, expected %d (stderr %r)"
                                 % (owner, result.returncode, exit_code,
                                    result.stderr))
        if result.stdout != b"":
            raise AssertionError("%s: stdout was not empty: %r"
                                 % (owner, result.stdout[:200]))
        if not result.stderr.startswith(("ERROR %s\n" % token).encode()):
            raise AssertionError("%s: stderr %r, expected ERROR %s"
                                 % (owner, result.stderr, token))
        return result

    def run(self, *arguments):
        # Only position 0 can be the option: a query TEXT may itself spell
        # `--disk-image`, and inspecting later positions would confuse data with
        # an option exactly as the tool must not.
        before = None
        image_path = None
        if len(arguments) >= 2 and arguments[0] == "--disk-image":
            candidate = Path(arguments[1])
            if candidate.is_file():
                image_path = candidate
                before = candidate.read_bytes()
        result = subprocess.run([self.tool, *map(str, arguments)],
                                capture_output=True, timeout=TIMEOUT,
                                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        if image_path is not None and before is not None:
            if image_path.read_bytes() != before:
                raise AssertionError("%s: the image changed" % (image_path,))
        return result

    def json_of(self, owner, *arguments):
        """A successful invocation whose stdout is exactly one JSON object + LF."""
        result = self.run(*arguments)
        if result.returncode != 0:
            raise AssertionError("%s: exit %d, stderr %r"
                                 % (owner, result.returncode, result.stderr))
        if not result.stdout.endswith(b"\n"):
            raise AssertionError("%s: stdout does not end with LF" % owner)
        body = result.stdout[:-1]
        if b"\n" in body:
            raise AssertionError("%s: stdout is not one line" % owner)
        decoded = json.loads(body.decode("utf-8"))
        if not isinstance(decoded, dict):
            raise AssertionError("%s: stdout is not a JSON object" % owner)
        return decoded

    def expect_error(self, owner, exit_code, token, *arguments):
        result = self.run(*arguments)
        if result.returncode != exit_code:
            raise AssertionError("%s: exit %d, expected %d (stderr %r)"
                                 % (owner, result.returncode, exit_code,
                                    result.stderr))
        if result.stdout != b"":
            raise AssertionError("%s: stdout was not empty: %r"
                                 % (owner, result.stdout[:200]))
        # The first line is the stable token; USAGE may add a short reminder of
        # the accepted forms after it.
        if not result.stderr.startswith(("ERROR %s\n" % token).encode()):
            raise AssertionError("%s: stderr %r, expected ERROR %s"
                                 % (owner, result.stderr, token))
        return result


def _concepts_by_id(payload):
    return {item["id"]: item for item in payload["concepts"]}


# ---------------------------------------------------------------------- cases

def case_list_schema_and_order(run):
    files = [
        ("z-note", "@block kind\n@kg relation is_a square rectangle\n条件在原文里。\n"
                   .encode("utf-8")),
        ("a-note", ("@yan-note 1\ntitle: 几何\n@end\n"
                    "@block definitions\n"
                    "@kg concept square 正方形\n"
                    "@kg concept rectangle 矩形\n"
                    "@kg alias square 等边矩形\n"
                    "@kg relation is_a square rectangle\n"
                    "@kg relation is_a square rectangle\n"
                    "@kg relation equivalent square square\n").encode("utf-8")),
        ("binary", b"@block nope\n@kg bad\n\x00"),
    ]
    path, data = run.image("healthy", files)
    expected_concepts, expected_relations, skipped = parse_notes(files)
    payload = run.json_of("list", "--disk-image", path, "list")
    assert set(payload) == {"version", "operation", "source_sha256", "skipped",
                            "total", "shown", "concepts"}, payload.keys()
    assert payload["version"] == 1
    assert payload["operation"] == "list"
    assert payload["source_sha256"] == source_digest(data)
    assert payload["skipped"] == skipped == 1, payload["skipped"]
    assert payload["total"] == len(expected_concepts)
    assert payload["shown"] == min(20, len(expected_concepts))
    ids = [item["id"] for item in payload["concepts"]]
    assert ids == sorted(ids), ids
    assert ids == [entry["id"] for entry in expected_concepts]
    by_id = _concepts_by_id(payload)
    for entry in expected_concepts:
        item = by_id[entry["id"]]
        assert set(item) == {"id", "name", "alias_count", "declaration_count",
                             "relation_count"}, item.keys()
        assert item["name"] == entry["name"]
        assert item["alias_count"] == len(entry["aliases"])
        assert item["declaration_count"] == len(entry["declarations"])
        related = [r for r in expected_relations
                   if r["subject"] == entry["id"] or r["object"] == entry["id"]]
        assert item["relation_count"] == len(related), (item, related)
    return path, data


def case_query_provenance_and_directions(run):
    text = ("@yan-note 1\ntitle: 几何\n@end\n"
            "@block definitions\r\n"
            "@kg concept square 正方形\n"
            "@kg concept rectangle 矩形\n"
            "@kg alias square 等边矩形\n"
            "@kg alias square 方\n"
            "@kg relation is_a square rectangle\n"
            "@kg relation part_of rectangle square\n"
            "@kg relation equivalent square square\n"
            "@kg relation is_a square rectangle\n")
    note = text.encode("utf-8")
    files = [("a-note", note)]
    path, data = run.image("query", files)
    expected_concepts, expected_relations, _ = parse_notes(files)
    payload = run.json_of("query", "--disk-image", path, "query", "正方形")
    assert set(payload) == {"version", "operation", "query", "source_sha256",
                            "skipped", "concept", "declarations", "relations"}
    assert payload["version"] == 1 and payload["operation"] == "query"
    assert payload["query"] == "正方形"
    assert payload["source_sha256"] == source_digest(data)
    assert payload["skipped"] == 0
    concept = payload["concept"]
    assert set(concept) == {"id", "name", "aliases"}
    assert concept["id"] == "square" and concept["name"] == "正方形"
    # The alias array is complete and label-sorted, never truncated.
    assert concept["aliases"] == sorted(["等边矩形", "方"]), concept["aliases"]

    declarations = payload["declarations"]
    assert set(declarations) == {"total", "shown", "items"}
    square = next(e for e in expected_concepts if e["id"] == "square")
    assert declarations["total"] == len(square["declarations"])
    assert declarations["shown"] == min(20, len(square["declarations"]))
    assert declarations["items"] == square["declarations"], (
        declarations["items"], square["declarations"])

    relations = payload["relations"]
    assert set(relations) == {"total", "shown", "items"}
    direct = [r for r in expected_relations
              if r["subject"] == "square" or r["object"] == "square"]
    assert relations["total"] == len(direct), (relations["total"], direct)
    assert relations["shown"] == min(20, len(direct))
    # Each row names both endpoints, its type, a direction and its full origin.
    for item in relations["items"]:
        assert set(item) == {"type", "subject", "object", "direction", "where"}
        assert set(item["subject"]) == {"id", "name"}
        assert set(item["object"]) == {"id", "name"}
        assert item["direction"] in {"out", "in", "self"}
        assert set(item["where"]) == {
            "file", "block_id", "declaration_line", "declaration_start",
            "declaration_end", "block_marker_line", "block_first_line",
            "block_last_line", "block_start", "block_end"}
    # Canonical order: file name ASCII, then declaration byte.
    order = [(item["where"]["file"], item["where"]["declaration_start"])
             for item in relations["items"]]
    assert order == sorted(order), order
    # The self loop is listed once and marked self; out/in are separated.
    self_rows = [i for i in relations["items"] if i["direction"] == "self"]
    assert len(self_rows) == 1, self_rows
    assert self_rows[0]["subject"] == self_rows[0]["object"]
    out_rows = [i for i in relations["items"] if i["direction"] == "out"]
    in_rows = [i for i in relations["items"] if i["direction"] == "in"]
    assert len(out_rows) == 2 and len(in_rows) == 1, (out_rows, in_rows)
    for item in out_rows:
        assert item["subject"]["id"] == "square"
    for item in in_rows:
        assert item["object"]["id"] == "square"
    # A relation's origin is the exact directive bytes, recoverable from it.
    # Every reported range is an offset into the NOTE file, which is the source
    # the specification means; the image holds it at a different place.
    for item, source in zip(relations["items"],
                            sorted(direct, key=lambda r: (r["file"],
                                                          r["declaration_start"]))):
        projected = {key: source[key] for key in item["where"]}
        assert item["where"] == projected, (item["where"], projected)
        raw = note[source["declaration_start"]:source["declaration_end"]]
        assert raw.startswith(b"@kg relation "), raw
        assert b"\r" not in raw and b"\n" not in raw, raw
        block = note[source["block_start"]:source["block_end"]]
        assert raw in block, (raw, block[:40])

    return path


def case_dash_prefixed_text(run):
    """A concept ID may contain `-` anywhere, including in first position.

    `--disk-image` and `-x` are legal identifiers, so a query TEXT that spells
    either must reach the graph as data. The option parser therefore only ever
    compares argv[1]; nothing after the operation word is interpreted.
    """
    files = [("a-note", ("@block a\n"
                         "@kg concept --disk-image 双\"引号\\路径\n"
                         "@kg concept -x 前导连字符\n"
                         "@kg alias --disk-image 说明\"\\\n"
                         "@kg concept plain 普通\n"
                         "@kg alias plain -别名\n").encode("utf-8"))]
    path, data = run.image("dash", files)
    expected_concepts, _, _ = parse_notes(files)
    by_id = {entry["id"]: entry for entry in expected_concepts}
    if set(by_id) != {"--disk-image", "-x", "plain"}:
        raise AssertionError("dash fixture: %r" % (sorted(by_id),))

    # A long option spelling as data resolves to the concept of that name.
    payload = run.json_of("dash-long", "--disk-image", path, "query",
                          "--disk-image")
    if payload["concept"]["id"] != "--disk-image":
        raise AssertionError("dash-long: %r" % (payload["concept"],))
    if payload["concept"]["name"] != '双"引号\\路径':
        raise AssertionError("dash-long name: %r" % (payload["concept"]["name"],))
    if payload["concept"]["aliases"] != ['说明"\\']:
        raise AssertionError("dash-long aliases: %r" % (payload["concept"]["aliases"],))
    if payload["declarations"]["items"] != by_id["--disk-image"]["declarations"]:
        raise AssertionError("dash-long provenance")
    if payload["source_sha256"] != source_digest(data):
        raise AssertionError("dash-long identity")

    # A short option spelling as data is equally a lookup, with the same result
    # as the exact ID of that concept.
    short = run.json_of("dash-short", "--disk-image", path, "query", "-x")
    if short["concept"]["id"] != "-x":
        raise AssertionError("dash-short: %r" % (short["concept"],))
    if short["concept"]["name"] != "前导连字符":
        raise AssertionError("dash-short name: %r" % (short["concept"]["name"],))

    # A dash-prefixed ALIAS resolves as an alias.
    alias = run.json_of("dash-alias", "--disk-image", path, "query", "-别名")
    if alias["concept"]["id"] != "plain":
        raise AssertionError("dash-alias: %r" % (alias["concept"],))

    # An absent dash-prefixed text is a lookup miss, never a usage error.
    run.expect_error("dash-missing", 1, "NOT_FOUND",
                     "--disk-image", path, "query", "-not-recorded")
    run.expect_error("dash-missing-long", 1, "NOT_FOUND",
                     "--disk-image", path, "query", "--absent-option")

    # A genuinely unknown word in the OPTION position is still USAGE.
    run.expect_error("dash-option-position", 2, "USAGE",
                     "--format", "json")
    run.expect_error("dash-option-position-2", 2, "USAGE",
                     "--disk-image", path, "list", "--extra")


def case_lookup_forms(run):
    files = [("a-note", ("@block p\n"
                         "@kg concept a Shared\n"
                         "@kg concept b Shared\n"
                         "@kg concept c Shared\n"
                         "@kg alias a 共同\n@kg alias b 共同\n@kg alias c 共同\n"
                         "@kg concept square 正方形\n"
                         "@kg alias square 等边矩形\n").encode("utf-8"))]
    path, _ = run.image("lookup", files)
    # Exact ID beats the ambiguous label of the same bytes.
    payload = run.json_of("lookup-id", "--disk-image", path, "query", "a")
    assert payload["concept"]["id"] == "a", payload["concept"]
    # A unique alias resolves.
    payload = run.json_of("lookup-alias", "--disk-image", path, "query", "等边矩形")
    assert payload["concept"]["id"] == "square", payload["concept"]
    # Three distinct names behind one label is AMBIGUOUS (never clamped to 2).
    run.expect_error("ambiguous-name", 1, "AMBIGUOUS",
                     "--disk-image", path, "query", "Shared")
    run.expect_error("ambiguous-alias", 1, "AMBIGUOUS",
                     "--disk-image", path, "query", "共同")
    run.expect_error("not-found", 1, "NOT_FOUND",
                     "--disk-image", path, "query", "absent")


def case_caps_and_first_twenty(run):
    # 23 concepts over 23 blocks, deliberately out of ASCII order in the file.
    lines = ["@block b%02d\n@kg concept c%02d 名称%02d\n" % (index, index, index)
             for index in reversed(range(23))]
    files = [("a-note", "".join(lines).encode("utf-8"))]
    path, data = run.image("caps", files)
    expected_concepts, _, _ = parse_notes(files)
    payload = run.json_of("caps-list", "--disk-image", path, "list")
    assert payload["total"] == 23 and payload["shown"] == 20
    assert [i["id"] for i in payload["concepts"]] == \
        [e["id"] for e in expected_concepts[:20]]

    # One concept with 23 relations to 23 different objects: total 23, shown 20.
    text = "@block defs\n@kg concept hub 中枢\n"
    for index in range(23):
        text += "@kg concept p%02d 端点%02d\n" % (index, index)
    for index in range(23):
        text += "@kg relation points_to hub p%02d\n" % index
    files = [("b-note", text.encode("utf-8"))]
    path, _ = run.image("caps-relations", files)
    payload = run.json_of("caps-query", "--disk-image", path, "query", "hub")
    assert payload["relations"]["total"] == 23
    assert payload["relations"]["shown"] == 20
    assert len(payload["relations"]["items"]) == 20
    starts = [i["where"]["declaration_start"] for i in payload["relations"]["items"]]
    assert starts == sorted(starts), starts
    assert payload["source_sha256"] == source_digest(
        (Path(path)).read_bytes())


def case_escaping(run):
    label = 'quote"back\\slash'
    text = ('@block p\n'
            '@kg concept weird ' + label + '\n'
            '@kg alias weird 中文别名\n').encode("utf-8")
    files = [("a-note", text)]
    path, _ = run.image("escape", files)
    payload = run.json_of("escape", "--disk-image", path, "query", "weird")
    assert payload["concept"]["name"] == label, payload["concept"]
    assert payload["concept"]["aliases"] == ["中文别名"]
    # The raw bytes on the wire must be valid JSON with the escapes applied and
    # the Chinese left as UTF-8, not \uXXXX.
    result = run.run("--disk-image", path, "query", "weird")
    assert '\\"' in result.stdout.decode("utf-8"), result.stdout
    assert "中文别名".encode("utf-8") in result.stdout, result.stdout


def case_text_edges(run):
    # CRLF everywhere, a final line without a newline, and a >16 KiB plain line.
    long_line = "长" * 6000  # 18000 bytes of UTF-8
    text = ("@block defs\r\n"
            "@kg concept square 正方形\r\n"
            + long_line + "\r\n"
            "@kg relation is_a square square").encode("utf-8")
    note = text
    files = [("a-note", note)]
    path, data = run.image("edges", files, blocks=16)
    expected_concepts, expected_relations, _ = parse_notes(files)
    payload = run.json_of("edges", "--disk-image", path, "query", "square")
    assert payload["declarations"]["total"] == 1
    assert payload["declarations"]["items"] == expected_concepts[0]["declarations"]
    assert payload["relations"]["total"] == 1
    assert payload["relations"]["items"][0]["direction"] == "self"
    where = payload["relations"]["items"][0]["where"]
    raw = note[where["declaration_start"]:where["declaration_end"]]
    assert raw == b"@kg relation is_a square square", raw
    # The 18 KiB prose line sits between the two directives inside one block.
    prose = note[where["block_start"]:where["block_end"]]
    assert len(prose) > 16384, len(prose)
    assert payload["source_sha256"] == source_digest(data)


def case_old_and_no_graph(run):
    files = [("old", "没有图声明的旧笔记，正文照常。\n".encode("utf-8")),
             ("empty", b""),
             ("skip", b"@block a\n@kg concept a A\n\x00")]
    path, data = run.image("old", files)
    payload = run.json_of("old", "--disk-image", path, "list")
    assert payload["total"] == 0 and payload["shown"] == 0
    assert payload["concepts"] == []
    assert payload["skipped"] == 1
    assert payload["source_sha256"] == source_digest(data)
    run.expect_error("old-query", 1, "NOT_FOUND",
                     "--disk-image", path, "query", "anything")


def case_build_errors(run):
    cases = {
        "metadata": ("@yan-note 1\n@block a\n@kg concept a A\n@end\n", "METADATA"),
        "blockformat": ("@block\n", "BLOCK_FORMAT"),
        "blockambiguous": ("@block a\n@block a\n", "BLOCK_AMBIGUOUS"),
        "blocklimit": ("".join("@block b%02d\n" % index for index in range(65)),
                       "BLOCK_LIMIT"),
        "graphformat": ("@kg concept a A\n", "GRAPH_FORMAT"),
        "graphformat-tab": ("@block a\n@kg\tconcept a A\n", "GRAPH_FORMAT"),
        "conflict": ("@block a\n@kg concept x 一\n@kg concept x 二\n", "CONFLICT"),
        "unresolved": ("@block a\n@kg relation t x y\n", "UNRESOLVED"),
    }
    for name, (text, token) in cases.items():
        path, _ = run.image("error-" + name, [("a-note", text.encode("utf-8"))])
        run.expect_error("error-" + name, 1, token, "--disk-image", path, "list")


def case_usage_and_inputs(run):
    path, _ = run.image("usage", [("a", b"@block x\n@kg concept a A\n")])
    absent = run.work / "absent.img"
    broken = run.work / "broken.img"
    broken.write_bytes(b"not an image at all")
    truncated = run.work / "truncated.img"
    truncated.write_bytes(b"\x00" * 8192)
    # Argument problems exit 2 with no image I/O at all: the image path here is
    # deliberately unusable, so any open would show up as a different token.
    for owner, arguments in [
        ("no-args", ()),
        ("no-command", ("--disk-image", absent)),
        ("unknown-command", ("--disk-image", absent, "remove")),
        ("missing-image-option", ("list",)),
        ("missing-text", ("--disk-image", absent, "query")),
        ("extra-word", ("--disk-image", absent, "list", "extra")),
        ("unknown-option", ("--disk-image", absent, "--format", "json", "list")),
        ("duplicate-option", ("--disk-image", absent, "--disk-image", absent,
                              "list")),
        ("empty-query", ("--disk-image", absent, "query", "")),
        ("newline-query", ("--disk-image", absent, "query", "a\nb")),
        ("control-query", ("--disk-image", absent, "query", "a\x7fb")),
        ("blank-query", ("--disk-image", absent, "query", "   ")),
        ("oversize-query", ("--disk-image", absent, "query", "q" * 257)),
    ]:
        run.expect_error(owner, 2, "USAGE", *arguments)
    # An invalid UTF-8 argv can only be produced with raw bytes; it is a usage
    # error, so this one call passes bytes instead of str.
    raw = subprocess.run([str(run.tool), b"--disk-image", str(absent).encode(),
                          b"query", b"a\xffb"], capture_output=True,
                         timeout=TIMEOUT)
    if raw.returncode != 2 or raw.stdout != b"" or \
            not raw.stderr.startswith(b"ERROR USAGE\n"):
        raise AssertionError("invalid-utf8-query: exit %d stderr %r"
                             % (raw.returncode, raw.stderr[:80]))
    # Real inputs that cannot be used are exit 1, never 2.
    run.expect_error("absent-image", 1, "IO",
                     "--disk-image", absent, "list")
    # A short file is refused by the adapter's size rule and a wrong header is a
    # bad format: both are content errors, not caller errors.
    run.expect_error("broken-image", 1, "CORRUPT",
                     "--disk-image", broken, "list")
    run.expect_error("truncated-image", 1, "CORRUPT",
                     "--disk-image", truncated, "list")
    odd = run.work / "odd.img"
    odd.write_bytes(b"\x00" * 5000)
    run.expect_error("odd-size-image", 1, "CORRUPT",
                     "--disk-image", odd, "list")
    # A directory opens but is not a regular image file, so it is a content
    # error rather than a caller error.
    run.expect_error("directory-image", 1, "CORRUPT",
                     "--disk-image", run.work, "list")
    # A healthy image still works, so the failures are about the input.
    run.json_of("usage-healthy", "--disk-image", path, "list")


def case_repeat_and_change(run):
    files = [("a-note", "@block p\n@kg concept square 正方形\n".encode("utf-8"))]
    path, data = run.image("repeat", files)
    first = run.json_of("repeat-1", "--disk-image", path, "list")
    second = run.json_of("repeat-2", "--disk-image", path, "list")
    assert first == second, (first, second)
    # A content change is a new identity and a new graph on the next invocation.
    changed = [("a-note", "@block p\n@kg concept square 正方形\n"
                          "@kg concept rectangle 矩形\n".encode("utf-8"))]
    (path).write_bytes(build_image(changed))
    third = run.json_of("repeat-3", "--disk-image", path, "list")
    assert third["total"] == 2 and third["source_sha256"] != first["source_sha256"]
    assert third["source_sha256"] == source_digest(path.read_bytes())
    # Renaming keeps the content but changes the identity.
    renamed = [("b-note", changed[0][1])]
    (path).write_bytes(build_image(renamed))
    fourth = run.json_of("repeat-4", "--disk-image", path, "list")
    assert fourth["source_sha256"] != third["source_sha256"]
    assert fourth["total"] == 2


def case_large_library(run):
    files = [("n%04d" % index,
              ("@block definition\n@kg concept c%04d 概念%d\n" % (index, index))
              .encode("utf-8"))
             for index in range(200)]
    path, data = run.image("large", files, capacity=255)
    expected_concepts, _, _ = parse_notes(files)
    payload = run.json_of("large", "--disk-image", path, "list")
    assert payload["total"] == 200 and payload["shown"] == 20
    assert [i["id"] for i in payload["concepts"]] == \
        [e["id"] for e in expected_concepts[:20]]
    assert payload["source_sha256"] == source_digest(data)
    last = run.json_of("large-last", "--disk-image", path, "query", "c0199")
    assert last["concept"]["id"] == "c0199"


def case_fault_paths(run):
    files = [("a-note", "@block p\n@kg concept square 正方形\n".encode("utf-8"))]
    path, _ = run.image("fault", files)
    # The injected digest comparison alone: no JSON, STALE.
    run.run_fault("stale", "stale", 1, "STALE",
                  "--disk-image", path, "list")
    run.run_fault("stale-query", "stale", 1, "STALE",
                  "--disk-image", path, "query", "square")
    # A failing first close outranks an otherwise successful build.
    run.run_fault("close", "close", 1, "IO", "--disk-image", path, "list")
    # A failing close of the second (post-observation) source is the same error:
    # the re-observation adapter must be closed successfully too.
    run.run_fault("close-second", "close-second", 1, "IO",
                  "--disk-image", path, "list")
    run.run_fault("close-second-query", "close-second", 1, "IO",
                  "--disk-image", path, "query", "square")
    # The probe is otherwise the real tool, so the same image still succeeds.
    healthy = subprocess.run([run.fault, "--disk-image", str(path), "list"],
                             capture_output=True, timeout=TIMEOUT,
                             env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    if healthy.returncode != 0 or not healthy.stdout.endswith(b"\n"):
        raise AssertionError("fault-probe healthy run: exit %d stderr %r"
                             % (healthy.returncode, healthy.stderr))


def case_real_source_change(run):
    """A genuinely changed image must be detected, not just a mocked digest.

    The injector renames one directory entry of a dedicated throw-away v1 image
    and recomputes its CRC after the tool's first source is closed. The tool's own
    second adapter then re-reads that directory block from disk, so STALE here
    comes from a different logical source rather than from a short-circuited
    comparison. Every assertion about the mutation is made here from the bytes.

    If the tool reused a cached directory, the second digest would equal the
    first and this case would see a success instead of STALE.
    """
    files = [("a-note", "@block p\n@kg concept square 正方形\n".encode("utf-8")),
             ("second", "@block p\n@kg concept rectangle 矩形\n".encode("utf-8"))]
    path, original = run.image("rewrite", files, blocks=16)
    before_digest = source_digest(original)
    before_names = _directory_names(original)

    result = run.run_fault("real-change", "rewrite", 1, "STALE",
                           "--disk-image", path, "list", image=path,
                           expect_image_change=True)

    # The injector must have made exactly the documented metadata change, and
    # the resealed image must still be a valid logical view.
    changed = path.read_bytes()
    if changed[:8] != MAGIC:
        raise AssertionError("real-change: injector damaged the magic")
    after_names = _directory_names(changed)
    if sorted(after_names) == sorted(before_names):
        raise AssertionError("real-change: no entry was renamed (%r)" % (after_names,))
    if len(after_names) != len(before_names):
        raise AssertionError("real-change: file count changed")
    for name in after_names:
        if name not in before_names and name not in [n for n in after_names]:
            raise AssertionError("real-change: unexpected name %r" % (name,))
    if _stored_crc(changed) != _crc32_directory(changed):
        raise AssertionError("real-change: injector did not reseal the CRC")
    after_digest = source_digest(changed)
    if after_digest == before_digest:
        raise AssertionError("real-change: the logical identity did not change")
    if result.stdout != b"":
        raise AssertionError("real-change: stdout was not empty")

    # A fresh invocation on the mutated (still valid) image succeeds with the new
    # identity, so the STALE above was about the change, not about damage.
    payload = run.json_of("real-change-after", "--disk-image", path, "list")
    if payload["source_sha256"] != after_digest:
        raise AssertionError("real-change-after: identity %r != %r"
                             % (payload["source_sha256"], after_digest))
    if payload["total"] != 2:
        raise AssertionError("real-change-after: total %r" % (payload["total"],))


def _directory_names(image):
    capacity = struct.unpack_from("<I", image, 20)[0]
    names = []
    for slot in range(capacity):
        entry = image[64 + slot * 64:128 + slot * 64]
        if entry[0] == 0:
            continue
        names.append(entry[:32].split(b"\x00", 1)[0].decode("ascii"))
    return names


def _stored_crc(image):
    return struct.unpack_from("<I", image, 60)[0]


def _crc32_directory(image):
    return zlib.crc32(image[:60] + b"\x00\x00\x00\x00" + image[64:4096]) & 0xFFFFFFFF


def _shell_quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def case_output_failure(run):
    """A real stdout failure must never be reported as success.

    Two genuine faults of the production binary are used, neither injected:

      * stdout closed before exec, so every write fails with EBADF;
      * /dev/full, where writes fail with ENOSPC -- used only when this host
        allows writing it, because a restricted device must not turn into a
        false failure.

    A prefix may already have reached the sink; what is forbidden is a zero exit
    or a missing diagnostic, exactly as the specification states.
    """
    files = [("a-note", "@block p\n@kg concept square 正方形\n".encode("utf-8"))]
    path, data = run.image("output", files)
    result = subprocess.run(
        "%s --disk-image %s list 1>&-"
        % (_shell_quote(run.tool), _shell_quote(str(path))),
        shell=True, capture_output=True, timeout=TIMEOUT,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    if result.returncode == 0:
        raise AssertionError("closed stdout was reported as success")
    if not result.stderr.startswith(b"ERROR "):
        raise AssertionError("closed stdout stderr %r" % result.stderr)
    if path.read_bytes() != data:
        raise AssertionError("the image changed")

    full = Path("/dev/full")
    writable = False
    if full.exists() and full.is_char_device():
        probe = subprocess.run("true > /dev/full", shell=True,
                               capture_output=True, timeout=TIMEOUT)
        writable = probe.returncode == 0
    if not writable:
        print("NOTE output_failure: /dev/full is not writable here; "
              "the closed-stdout fault already covers the contract")
        return
    result = subprocess.run(
        "%s --disk-image %s list > /dev/full"
        % (_shell_quote(run.tool), _shell_quote(str(path))),
        shell=True, capture_output=True, timeout=TIMEOUT,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    if result.returncode == 0:
        raise AssertionError("/dev/full was reported as success")
    if not result.stderr.startswith(b"ERROR "):
        raise AssertionError("/dev/full stderr %r" % result.stderr)
    if path.read_bytes() != data:
        raise AssertionError("the image changed")


CASES = (
    ("list_schema_and_order", case_list_schema_and_order),
    ("query_provenance_and_directions", case_query_provenance_and_directions),
    ("dash_prefixed_text", case_dash_prefixed_text),
    ("lookup_forms", case_lookup_forms),
    ("caps_and_first_twenty", case_caps_and_first_twenty),
    ("escaping", case_escaping),
    ("text_edges", case_text_edges),
    ("old_and_no_graph", case_old_and_no_graph),
    ("build_errors", case_build_errors),
    ("usage_and_inputs", case_usage_and_inputs),
    ("repeat_and_change", case_repeat_and_change),
    ("large_library", case_large_library),
    ("fault_paths", case_fault_paths),
    ("real_source_change", case_real_source_change),
    ("output_failure", case_output_failure),
)

INPUTS = (
    "tests/yan_graph_checks.py",
    "host/knowledge_graph.h",
    "host/knowledge_graph.c",
    "tools/yan_graph.c",
    "tests/test_knowledge_graph.c",
    "tests/yan_graph_fault.c",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, help="the real yan_graph binary")
    parser.add_argument("--fault", default=None,
                        help="test-only fault probe binary (STALE/close)")
    parser.add_argument("--source", default=None)
    parser.add_argument("--case", default=None)
    arguments = parser.parse_args()
    if arguments.source:
        for name in INPUTS:
            if not (Path(arguments.source) / name).exists():
                print(":FAIL: 0044_r2_inputs: missing %s" % name)
                return 1
    if not Path(arguments.tool).exists():
        print(":FAIL: 0044_r2_tool: missing %s" % arguments.tool)
        return 1
    selected = [(name, function) for name, function in CASES
                if arguments.case is None or name == arguments.case]
    if arguments.case is not None and not selected:
        print(":FAIL: 0044_r2_case: unknown case %s" % arguments.case)
        return 1
    run = Runner(arguments.tool, arguments.fault)
    failures = 0
    for name, function in selected:
        try:
            function(run)
            print("PASS %s" % name)
        except AssertionError as error:
            failures += 1
            print(":FAIL: 0044_r2_%s: %s" % (name, error))
        except subprocess.TimeoutExpired:
            raise HarnessError("case %s timed out" % name)
        except (OSError, ValueError) as error:
            raise HarnessError("case %s: %s" % (name, error))
    print("%s: %d/%d cases" % ("FAIL" if failures else "OK",
                               len(selected) - failures, len(selected)))
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print(":FAIL: 0044_r2_harness: %s" % error)
        sys.exit(2)
