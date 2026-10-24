"""Independent 0036-R8 historical parity fixture and comparators.

This module is pure Python (no POSIX, no Git, no compilers, no product
headers) so it can be imported by the Windows default gate and by the
operator-only Linux runner. It owns one recipe: the historical N=63/1023/2047
images, their decoded invariants, the two CRC-valid corruption fixtures, the
pre-R4 terminal receipts, and the MCP v1/v2 oracles.

A failure is always a ParityFailure with a stable code and field. Callers
(and the default gate) match both; a bare assertion is never the contract.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

BLOCK = 4096
HEADER = 64
ENTRY = 64
NAME_MAX = 31
MAGIC = b"YANFS01\x00"

REF_BASELINE = "4c5e0ab1a036e845123e304333d363478243c146"
TREE_BASELINE = "eb284e7cf94c2e4e7aa442583004f1041e8beb6f"
REF_CANDIDATE = "2b82d17065bbd5b83e2d5e2f201f37f5308dedf5"
TREE_CANDIDATE = "a6daeffc8a713dfaf3ca1b249a43f7d6aa14d953"

SIZES = (63, 1023, 2047)
NORMAL_CASES = ("mount", "create", "ls", "meta", "backlinks",
                "related", "block-backlinks", "search")
CORRUPT_CASES = ("duplicate", "overlap")
MCP_CASE = "mcp"

NOTE = (b"@yan-note 1\nlink: f0000.txt\ntag: t\n@end\n"
        b"@block x\n[[f0000.txt#x]]\n")
PLAIN = b"a" * 60
TRAILER = b"yanfs> exit\r\nOK exit\r\nyanfs: exit\r\n"
CORRUPT_CAPTURE = b"ERROR CORRUPT\r\n"
CORRUPT_TOHOST = 0x71000006
CAPTURE_CAPACITY = 65536

_HEX64 = re.compile(r"^[0-9a-f]{64}$")


class ParityFailure(Exception):
    """A specific comparison/oracle failure, not a generic assert."""

    def __init__(self, code: str, field: str, detail: str = ""):
        self.code = code
        self.field = field
        self.detail = detail
        super().__init__("%s [%s]: %s" % (code, field, detail))


@dataclass
class TerminalObservation:
    capture: bytes
    exit: int
    status: dict
    image_before: bytes
    image_after: bytes
    stdout: bytes = b""
    stderr: bytes = b""


@dataclass
class McpObservation:
    raw: bytes
    reply: object
    exit: int
    image_before: bytes
    image_after: bytes


# --------------------------------------------------------------- case matrix

def case_matrix():
    """The exact 33 pair cases: 24 terminal, 6 corrupt, 3 MCP."""
    cases = []
    for files in SIZES:
        for case in NORMAL_CASES:
            cases.append((files, case))
    for files in SIZES:
        for case in CORRUPT_CASES:
            cases.append((files, case))
    for files in SIZES:
        cases.append((files, MCP_CASE))
    return tuple(cases)


def assert_case_matrix(cases):
    expected = list(case_matrix())
    seen = list(cases)
    if not seen:
        raise ParityFailure("MATRIX_LENGTH", "matrix", "zero cases")
    if len(set(seen)) != len(seen):
        raise ParityFailure("MATRIX_DUPLICATE", "matrix", "duplicate case")
    missing = [item for item in expected if item not in seen]
    if missing:
        raise ParityFailure("MATRIX_MISSING", "matrix", repr(missing[0]))
    extra = [item for item in seen if item not in expected]
    if extra:
        raise ParityFailure("MATRIX_EXTRA", "matrix", repr(extra[0]))
    if len(seen) != len(expected):
        raise ParityFailure("MATRIX_LENGTH", "matrix",
                            "got %d cases, expected %d" % (len(seen), len(expected)))
    return expected


# ----------------------------------------------------------- image encoding

def directory_blocks(files: int) -> int:
    if files not in SIZES:
        raise ParityFailure("FIXTURE_SIZE", "files", repr(files))
    blocks = (files + 1) // 64
    if blocks < 1 or blocks > 64 or blocks * 64 - 1 != files:
        raise ParityFailure("FIXTURE_SIZE", "files", repr(files))
    return blocks


def slot_names(files: int):
    """Physical slot order: f0000 first, then f(N-1) .. f0001."""
    directory_blocks(files)
    result = ["f0000.txt"]
    result.extend("f%04d.txt" % (files - slot) for slot in range(1, files))
    return result


def plain_slot(files: int) -> int:
    directory_blocks(files)
    return files // 2


def plain_name(files: int) -> str:
    return slot_names(files)[plain_slot(files)]


def entry_location(slot: int, blocks: int):
    if slot < 63:
        return 0, HEADER + slot * ENTRY
    if slot >= blocks * 64 - 1:
        raise ParityFailure("ENTRY_SLOT", "slot", repr(slot))
    index = slot - 63
    return 1 + index // 64, (index % 64) * ENTRY


def _u32(image, offset: int) -> int:
    return struct.unpack_from("<I", image, offset)[0]


def _put_u32(image, offset: int, value: int):
    struct.pack_into("<I", image, offset, value & 0xFFFFFFFF)


def _content(files: int, slot: int) -> bytes:
    return PLAIN if slot == plain_slot(files) else NOTE


def encode(files: int) -> bytes:
    blocks = directory_blocks(files)
    capacity = files + blocks + 4
    image = bytearray(capacity * BLOCK)
    image[0:8] = MAGIC
    _put_u32(image, 8, 1 if blocks == 1 else 2)
    _put_u32(image, 12, BLOCK)
    _put_u32(image, 16, capacity)
    _put_u32(image, 20, files)
    if blocks > 1:
        _put_u32(image, 24, blocks)
    for slot, name in enumerate(slot_names(files)):
        encoded = name.encode("ascii")
        if not 1 <= len(encoded) <= NAME_MAX:
            raise ParityFailure("FIXTURE_NAME", "name", name)
        data = _content(files, slot)
        block, offset = entry_location(slot, blocks)
        base = block * BLOCK + offset
        image[base:base + ENTRY] = b"\x00" * ENTRY
        image[base:base + len(encoded)] = encoded
        start = blocks + slot
        _put_u32(image, base + 32, len(data))
        _put_u32(image, base + 36, start)
        _put_u32(image, base + 40, 1)
        image[start * BLOCK:start * BLOCK + len(data)] = data
    return recompute_crc(image)


def recompute_crc(image) -> bytes:
    data = bytearray(image)
    if len(data) < BLOCK or len(data) % BLOCK:
        raise ParityFailure("IMAGE_SIZE", "image", str(len(data)))
    version = _u32(data, 8)
    blocks = 1 if version == 1 else _u32(data, 24)
    if blocks < 1 or blocks * BLOCK > len(data):
        raise ParityFailure("HEADER_BLOCKS", "directory_blocks", repr(blocks))
    data[60:64] = b"\x00" * 4
    crc = zlib.crc32(bytes(data[:blocks * BLOCK])) & 0xFFFFFFFF
    _put_u32(data, 60, crc)
    return bytes(data)


def decode(image: bytes) -> dict:
    if len(image) < BLOCK or len(image) % BLOCK:
        raise ParityFailure("IMAGE_SIZE", "image", str(len(image)))
    if image[0:8] != MAGIC:
        raise ParityFailure("HEADER_MAGIC", "magic", image[0:8].hex())
    version = _u32(image, 8)
    block_size = _u32(image, 12)
    capacity = _u32(image, 16)
    entry_capacity = _u32(image, 20)
    if version == 1:
        blocks = 1
    elif version == 2:
        blocks = _u32(image, 24)
    else:
        raise ParityFailure("HEADER_VERSION", "version", repr(version))
    if block_size != BLOCK:
        raise ParityFailure("HEADER_BLOCK_SIZE", "block_size", repr(block_size))
    if capacity != len(image) // BLOCK:
        raise ParityFailure("HEADER_CAPACITY", "capacity", repr(capacity))
    if blocks < 1 or blocks > 64 or entry_capacity != blocks * 64 - 1:
        raise ParityFailure("HEADER_ENTRY_CAPACITY", "entry_capacity",
                            repr(entry_capacity))
    if capacity < blocks or (blocks > 1 and capacity == blocks):
        raise ParityFailure("HEADER_CAPACITY", "capacity", repr(capacity))
    reserved = image[24:60]
    if version == 2:
        if _u32(image, 24) != blocks or any(reserved[4:]):
            raise ParityFailure("HEADER_RESERVED", "reserved", reserved.hex())
    elif any(reserved):
        raise ParityFailure("HEADER_RESERVED", "reserved", reserved.hex())
    stored = _u32(image, 60)
    crc_image = bytearray(image[:blocks * BLOCK])
    crc_image[60:64] = b"\x00" * 4
    if (zlib.crc32(bytes(crc_image)) & 0xFFFFFFFF) != stored:
        raise ParityFailure("CRC_MISMATCH", "crc", "%08x" % stored)
    entries = []
    for slot in range(entry_capacity):
        block, offset = entry_location(slot, blocks)
        base = block * BLOCK + offset
        raw_name = image[base:base + 32].split(b"\x00", 1)[0]
        if not raw_name:
            entries.append(None)
            continue
        try:
            name = raw_name.decode("ascii")
        except UnicodeDecodeError as error:
            raise ParityFailure("ENTRY_NAME", "name", str(error)) from error
        size = _u32(image, base + 32)
        start = _u32(image, base + 36)
        count = _u32(image, base + 40)
        entries.append({"slot": slot, "name": name, "size": size,
                        "start": start, "count": count})
    return {"version": version, "directory_blocks": blocks,
            "capacity_blocks": capacity, "entry_capacity": entry_capacity,
            "crc": stored, "entries": entries}


def check_unique_names(entries):
    seen = set()
    for entry in entries:
        if entry is None:
            continue
        if entry["name"] in seen:
            raise ParityFailure("DUPLICATE_NAME", "name", entry["name"])
        seen.add(entry["name"])
    return True


def check_extents(entries, blocks: int, capacity: int):
    used = [entry for entry in entries
            if entry is not None and entry["size"] > 0]
    if not used:
        raise ParityFailure("NO_EXTENTS", "extents", "no non-empty entry")
    for entry in used:
        if entry["count"] == 0 or entry["start"] < blocks or \
                entry["start"] + entry["count"] > capacity:
            raise ParityFailure("EXTENT_RANGE", "extent", entry["name"])
    ordered = sorted(used, key=lambda item: item["start"])
    if ordered[0]["start"] != blocks:
        raise ParityFailure("EXTENT_NOT_CONTIGUOUS", "extents",
                            "first start %d" % ordered[0]["start"])
    for left, right in zip(ordered, ordered[1:]):
        end = left["start"] + left["count"]
        if end > right["start"]:
            raise ParityFailure("EXTENT_OVERLAP", "extents",
                                "%s/%s" % (left["name"], right["name"]))
        if end < right["start"]:
            raise ParityFailure("EXTENT_NOT_CONTIGUOUS", "extents",
                                "%s/%s" % (left["name"], right["name"]))
    return True


def validate_fixture(files: int, image: bytes) -> dict:
    blocks = directory_blocks(files)
    info = decode(image)
    if info["version"] != (1 if blocks == 1 else 2):
        raise ParityFailure("FIXTURE_VERSION", "version", repr(info["version"]))
    if info["directory_blocks"] != blocks:
        raise ParityFailure("FIXTURE_DIR_BLOCKS", "directory_blocks",
                            repr(info["directory_blocks"]))
    if info["capacity_blocks"] != files + blocks + 4:
        raise ParityFailure("FIXTURE_CAPACITY", "capacity",
                            repr(info["capacity_blocks"]))
    entries = info["entries"]
    if len(entries) != files or any(entry is None for entry in entries):
        raise ParityFailure("FIXTURE_SLOTS", "entries", str(len(entries)))
    names = slot_names(files)
    if [entry["name"] for entry in entries] != names:
        raise ParityFailure("FIXTURE_NAMES", "names", "slot order mismatch")
    check_unique_names(entries)
    check_extents(entries, blocks, info["capacity_blocks"])
    if names[0] != "f0000.txt":
        raise ParityFailure("FIXTURE_TARGET", "names", names[0])
    if names == sorted(names):
        raise ParityFailure("FIXTURE_PERMUTATION", "names",
                            "name order equals slot order")
    for slot, entry in enumerate(entries):
        data = image[entry["start"] * BLOCK:
                     entry["start"] * BLOCK + entry["count"] * BLOCK]
        content = _content(files, slot)
        if entry["size"] != len(content):
            raise ParityFailure("FIXTURE_SIZE_FIELD", "size", entry["name"])
        if data[:len(content)] != content:
            raise ParityFailure("FIXTURE_CONTENT", "content", entry["name"])
        if any(data[len(content):]):
            raise ParityFailure("FIXTURE_CONTENT_TAIL", "content", entry["name"])
    digest = hashlib.sha256(image).hexdigest()
    canonical = json.dumps([{key: entry[key] for key in
                             ("slot", "name", "size", "start", "count")}
                            for entry in entries], sort_keys=True,
                           separators=(",", ":")).encode("utf-8")
    return {"files": files, "directory_blocks": blocks,
            "capacity_blocks": info["capacity_blocks"], "crc": info["crc"],
            "image_sha256": digest, "entries_sha256":
            hashlib.sha256(canonical).hexdigest(),
            "contiguous": True, "name_order_permuted": True,
            "plain_slot": plain_slot(files), "plain_name": plain_name(files),
            "names_sha256": hashlib.sha256(
                "\n".join(names).encode("utf-8")).hexdigest()}


def corrupt_duplicate(image: bytes) -> bytes:
    info = decode(image)
    entries = info["entries"]
    files = info["entry_capacity"]
    last = entries[-1]
    if last is None:
        raise ParityFailure("CORRUPTION_FAILED", "duplicate", "last slot empty")
    name = entries[0]["name"].encode("ascii")
    block, offset = entry_location(files - 1, info["directory_blocks"])
    base = block * BLOCK + offset
    data = bytearray(image)
    data[base:base + ENTRY] = b"\x00" * ENTRY
    data[base:base + len(name)] = name
    result = recompute_crc(data)
    duplicate = decode(result)
    try:
        check_unique_names(duplicate["entries"])
    except ParityFailure as error:
        if error.code != "DUPLICATE_NAME":
            raise
        return result
    raise ParityFailure("CORRUPTION_FAILED", "duplicate", "still unique")


def corrupt_overlap(image: bytes) -> bytes:
    info = decode(image)
    entries = info["entries"]
    files = info["entry_capacity"]
    last = entries[-1]
    if last is None:
        raise ParityFailure("CORRUPTION_FAILED", "overlap", "last slot empty")
    start = entries[0]["start"]
    block, offset = entry_location(files - 1, info["directory_blocks"])
    base = block * BLOCK + offset
    data = bytearray(image)
    _put_u32(data, base + 36, start)
    result = recompute_crc(data)
    overlap = decode(result)
    try:
        check_extents(overlap["entries"], overlap["directory_blocks"],
                      overlap["capacity_blocks"])
    except ParityFailure as error:
        if error.code != "EXTENT_OVERLAP":
            raise
        return result
    raise ParityFailure("CORRUPTION_FAILED", "overlap", "still disjoint")


# --------------------------------------------------------------- receipts

def _command_line(files: int, case: str) -> bytes:
    if case == "mount":
        return b"exit"
    if case == "create":
        return (b"rm " + plain_name(files).encode("ascii") + b"\ncreate " +
                plain_name(files).encode("ascii") + b" " + PLAIN)
    return {"ls": b"ls", "meta": b"meta list",
            "backlinks": b"backlinks f0000.txt",
            "related": b"related f0000.txt",
            "block-backlinks": b"block backlinks f0000.txt#x",
            "search": b"search x"}[case]


def stdin_for(files: int, case: str) -> bytes:
    if case == "create":
        return (b"rm " + plain_name(files).encode("ascii") + b"\ncreate " +
                plain_name(files).encode("ascii") + b" " + PLAIN +
                b"\nexit\n")
    return _command_line(files, case) + b"\n" + (b"" if case == "mount"
                                                  else b"exit\n")


def expected_terminal(files: int, case: str) -> dict:
    names = slot_names(files)
    plain = plain_name(files)
    notes = sorted(name for name in names if name != plain)
    related = ["f%04d.txt" % index for index in range(1, 21)]
    block = ["f%04d.txt" % index for index in range(0, 20)]
    if case == "ls":
        lines = "".join("%d %s\r\n" % (60 if name == plain else 65, name)
                        for name in names).encode("ascii")
        receipt = b"OK ls\r\n"
        kind = "body"
    elif case == "meta":
        lines = "".join(name + "\r\n" for name in sorted(names)).encode("ascii")
        receipt = ("OK meta list shown=%d\r\n" % files).encode("ascii")
        kind = "body"
    elif case == "backlinks":
        lines = "".join("BACKLINK " + name + "\r\n" for name in notes).encode("ascii")
        receipt = ("OK backlinks shown=%d target=OK\r\n" % (files - 1)).encode("ascii")
        kind = "body"
    elif case == "related":
        lines = "".join("RELATED %s out=0 in=1 tags=1\r\n" % name
                        for name in related).encode("ascii")
        receipt = ("OK related total=%d shown=20\r\n" % (files - 2)).encode("ascii")
        kind = "body"
    elif case == "block-backlinks":
        lines = "".join("BACKLINK source=%s block=#x line=6 bytes=49:64 "
                        "target=f0000.txt#x\r\n" % name
                        for name in block).encode("ascii")
        receipt = ("OK block backlinks total=%d shown=20 skipped=0 target=OK\r\n"
                   % (files - 1)).encode("ascii")
        kind = "body"
    elif case == "search":
        pairs = []
        for name in names[:10]:
            pairs.append("%s:5:@block x\r\n" % name)
            pairs.append("%s:6:[[f0000.txt#x]]\r\n" % name)
        lines = "".join(pairs).encode("ascii")
        receipt = ("OK search total=%d shown=20 skipped=0 mode=index\r\n"
                   % (2 * (files - 1))).encode("ascii")
        kind = "search"
    elif case == "mount":
        lines = b""
        receipt = b"OK exit\r\n"
        kind = "mount"
    elif case == "create":
        lines = b""
        receipt = b"OK create\r\n"
        kind = "create"
    else:
        raise ParityFailure("CASE_UNKNOWN", "case", case)
    return {"files": files, "case": case, "kind": kind,
            "stdin": stdin_for(files, case), "receipt": receipt,
            "receipts": ([b"OK rm\r\n", b"OK create\r\n", b"OK exit\r\n"]
                         if case == "create" else [receipt]),
            "body": lines, "trailer": TRAILER, "names": names,
            "plain_slot": plain_slot(files), "plain_name": plain,
            "related_names": related, "block_names": block,
            "search_total": 2 * (files - 1)}


def expected_corrupt(files: int) -> dict:
    return {"files": files, "case": "corrupt", "capture": CORRUPT_CAPTURE,
            "exit": 6, "verdict": "guest", "tohost": CORRUPT_TOHOST,
            "stdin": b"exit\n"}


def expected_mcp(files: int) -> dict:
    if files == 63:
        return {"files": files, "mode": "success", "total": 62, "shown": 20,
                "skipped": 0, "target_state": "OK", "name": "f0000.txt",
                "block_id": "x", "line": 6, "start": 49, "end": 64,
                "backlinks": ["f%04d.txt" % index for index in range(0, 20)]}
    if files in (1023, 2047):
        return {"files": files, "mode": "error_parity", "code": "CORRUPT"}
    raise ParityFailure("MCP_SIZE", "files", repr(files))


# ---------------------------------------------------------- terminal oracle

def _between(capture: bytes, start: bytes, end: bytes, code: str):
    at = capture.find(start)
    if at < 0:
        raise ParityFailure("MISSING_ECHO", "capture", start.decode("ascii", "replace"))
    at += len(start)
    stop = capture.find(end, at)
    if stop < 0:
        raise ParityFailure("MISSING_RECEIPT", "capture",
                            end.decode("ascii", "replace"))
    return capture[at:stop]


def check_terminal_observation(observation: TerminalObservation, expected: dict,
                               expected_image: bytes):
    capture = observation.capture
    status = observation.status
    if not capture:
        raise ParityFailure("EMPTY_CAPTURE", "capture", "no output")
    if len(capture) >= CAPTURE_CAPACITY - 1:
        raise ParityFailure("CAPTURE_TRUNCATED", "capture", str(len(capture)))
    if int(status.get("out_length", -1)) != len(capture):
        raise ParityFailure("OUT_LENGTH_MISMATCH", "out_length",
                            "%r != %d" % (status.get("out_length"), len(capture)))
    if int(status.get("delivered", -1)) != len(capture):
        raise ParityFailure("DELIVERED_MISMATCH", "delivered",
                            "%r != %d" % (status.get("delivered"), len(capture)))
    if status.get("verdict") != "pass":
        raise ParityFailure("STATUS_INVALID", "verdict", repr(status.get("verdict")))
    if int(status.get("_tohost", -1)) != 1:
        raise ParityFailure("TOHOST_INVALID", "tohost", repr(status.get("_tohost")))
    if observation.exit != 0:
        raise ParityFailure("EXIT_INVALID", "exit", repr(observation.exit))
    if observation.image_before != expected_image:
        raise ParityFailure("IMAGE_BEFORE_MISMATCH", "image_before", "input changed")
    if observation.image_after != expected_image:
        raise ParityFailure("IMAGE_CHANGED", "image_sha256",
                            hashlib.sha256(observation.image_after).hexdigest())
    if not capture.startswith(b"yanfs terminal\r\n"):
        raise ParityFailure("MISSING_BANNER", "capture", "no banner")
    if not capture.endswith(expected["trailer"]):
        raise ParityFailure("MISSING_TRAILER", "capture", "no exit trailer")
    for receipt in expected["receipts"]:
        if receipt not in capture:
            raise ParityFailure("MISSING_RECEIPT", "capture",
                                receipt.decode("ascii", "replace"))
    if b"ERROR " in capture:
        raise ParityFailure("UNEXPECTED_ERROR", "capture", "ERROR present")
    case = expected["case"]
    if case == "mount":
        want = b"yanfs terminal\r\n" + TRAILER
        if capture != want:
            raise ParityFailure("MOUNT_CAPTURE", "capture", "not exactly banner+exit")
        return
    if case == "create":
        for receipt in (b"OK rm\r\n", b"OK create\r\n"):
            if receipt not in capture:
                raise ParityFailure("MISSING_RECEIPT", "capture",
                                    receipt.decode("ascii"))
        info = decode(observation.image_after)
        entry = info["entries"][expected["plain_slot"]]
        if entry is None or entry["name"] != expected["plain_name"] or \
                entry["size"] != len(PLAIN) or \
                entry["start"] != info["directory_blocks"] + expected["plain_slot"] or \
                entry["count"] != 1:
            raise ParityFailure("CREATE_ENTRY", "entry", repr(entry))
    elif case == "ls":
        body = _between(capture, b"yanfs> ls\r\n", b"OK ls\r\n", "ls")
        if body != expected["body"]:
            raise ParityFailure("LS_LINES", "ls", "directory order/content")
    elif case == "meta":
        body = _between(capture, b"yanfs> meta list\r\n",
                        expected["receipt"], "meta")
        if body != expected["body"]:
            raise ParityFailure("META_NAMES", "meta", "name order/content")
    elif case == "backlinks":
        body = _between(capture, b"yanfs> backlinks f0000.txt\r\n",
                        expected["receipt"], "backlinks")
        if body != expected["body"]:
            raise ParityFailure("BACKLINKS", "backlinks", "name order/content")
    elif case == "related":
        body = _between(capture, b"yanfs> related f0000.txt\r\n",
                        expected["receipt"], "related")
        if body != expected["body"]:
            raise ParityFailure("RELATED_NAMES", "related", "top-20 or tie order")
    elif case == "block-backlinks":
        body = _between(capture, b"yanfs> block backlinks f0000.txt#x\r\n",
                        expected["receipt"], "block-backlinks")
        if body != expected["body"]:
            raise ParityFailure("BLOCK_BACKLINKS", "block-backlinks",
                                "source order/content")
    elif case == "search":
        body = _between(capture, b"yanfs> search x\r\n", expected["receipt"],
                        "search")
        if body != expected["body"]:
            raise ParityFailure("SEARCH_RESULTS", "search",
                                "result rows/names/order")
    else:
        raise ParityFailure("CASE_UNKNOWN", "case", case)


def compare_terminal(left: TerminalObservation, right: TerminalObservation,
                     expected: dict, expected_image: bytes):
    if not left.capture or not right.capture:
        raise ParityFailure("EMPTY_CAPTURE", "capture", "one side empty")
    if left.capture != right.capture:
        raise ParityFailure("CAPTURE_MISMATCH", "capture",
                            "first difference at %d" % _first_diff(
                                left.capture, right.capture))
    if len(left.capture) != len(right.capture):
        raise ParityFailure("CAPTURE_LENGTH_MISMATCH", "capture", "length")
    if left.exit != right.exit:
        raise ParityFailure("EXIT_MISMATCH", "exit", "%d != %d"
                            % (left.exit, right.exit))
    if left.status.get("verdict") != right.status.get("verdict"):
        raise ParityFailure("VERDICT_MISMATCH", "verdict",
                            "%r != %r" % (left.status.get("verdict"),
                                          right.status.get("verdict")))
    if left.status.get("_tohost") != right.status.get("_tohost"):
        raise ParityFailure("TOHOST_MISMATCH", "tohost",
                            "%r != %r" % (left.status.get("_tohost"),
                                          right.status.get("_tohost")))
    if left.image_before != right.image_before:
        raise ParityFailure("IMAGE_BEFORE_MISMATCH", "image_before", "inputs differ")
    if left.image_after != right.image_after:
        raise ParityFailure("IMAGE_MISMATCH", "image_sha256", "finals differ")
    check_terminal_observation(left, expected, expected_image)
    check_terminal_observation(right, expected, expected_image)
    return expected


def check_corrupt_observation(observation: TerminalObservation,
                              expected_image: bytes):
    capture = observation.capture
    if not capture:
        raise ParityFailure("EMPTY_CAPTURE", "capture", "no output")
    if capture != CORRUPT_CAPTURE:
        raise ParityFailure("CORRUPT_CAPTURE", "capture", capture[:32].hex())
    if observation.exit != 6:
        raise ParityFailure("CORRUPT_EXIT", "exit", repr(observation.exit))
    if observation.status.get("verdict") != "guest":
        raise ParityFailure("CORRUPT_VERDICT", "verdict",
                            repr(observation.status.get("verdict")))
    if int(observation.status.get("_tohost", -1)) != CORRUPT_TOHOST:
        raise ParityFailure("CORRUPT_TOHOST", "tohost",
                            repr(observation.status.get("_tohost")))
    if int(observation.status.get("out_length", -1)) != len(capture):
        raise ParityFailure("CORRUPT_OUT_LENGTH", "out_length", "capture length")
    if int(observation.status.get("delivered", -1)) != len(capture):
        raise ParityFailure("CORRUPT_DELIVERED", "delivered", "capture length")
    if observation.image_before != expected_image:
        raise ParityFailure("CORRUPT_IMAGE_BEFORE", "image_before", "input changed")
    if observation.image_after != expected_image:
        raise ParityFailure("CORRUPT_IMAGE", "image_sha256", "image changed")


def compare_corrupt(left: TerminalObservation, right: TerminalObservation,
                    expected_image: bytes):
    if not left.capture or not right.capture:
        raise ParityFailure("EMPTY_CAPTURE", "capture", "one side empty")
    if left.capture != right.capture:
        raise ParityFailure("CAPTURE_MISMATCH", "capture", "corrupt output")
    if left.exit != right.exit:
        raise ParityFailure("EXIT_MISMATCH", "exit", "corrupt exit")
    if left.status.get("verdict") != right.status.get("verdict"):
        raise ParityFailure("VERDICT_MISMATCH", "verdict", "corrupt verdict")
    if left.status.get("_tohost") != right.status.get("_tohost"):
        raise ParityFailure("TOHOST_MISMATCH", "tohost", "corrupt tohost")
    if left.image_after != right.image_after:
        raise ParityFailure("IMAGE_MISMATCH", "image_sha256", "corrupt image")
    check_corrupt_observation(left, expected_image)
    check_corrupt_observation(right, expected_image)
    return True


def _first_diff(left: bytes, right: bytes) -> int:
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            return index
    return min(len(left), len(right))


# --------------------------------------------------------------- MCP oracle

def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ParityFailure("MCP_DUPLICATE_KEY", "json", key)
        result[key] = value
    return result


def _load(raw: bytes):
    if not raw:
        raise ParityFailure("MCP_EMPTY", "raw", "no response line")
    if b"\n" in raw or b"\r" in raw:
        raise ParityFailure("MCP_WIRE_NEWLINE", "raw", "more than one line")
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except ParityFailure:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ParityFailure("MCP_JSON", "raw", str(error)) from error


def _keys(value, want, field: str):
    if not isinstance(value, dict) or set(value) != set(want):
        got = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise ParityFailure("MCP_SCHEMA", field, repr(got))
    return value


def _int(value, field: str):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ParityFailure("MCP_TYPE", field, type(value).__name__)
    return value


def _str(value, field: str):
    if not isinstance(value, str):
        raise ParityFailure("MCP_TYPE", field, type(value).__name__)
    return value


def _server_info(result):
    meta = result.get("_meta")
    if not isinstance(meta, dict) or \
            "io.modelcontextprotocol/serverInfo" not in meta:
        raise ParityFailure("MCP_SCHEMA", "_meta.serverInfo", "missing")
    info = meta["io.modelcontextprotocol/serverInfo"]
    _keys(info, ("name", "version"), "_meta.serverInfo")
    if info["name"] != "YanOS" or not isinstance(info["version"], str):
        raise ParityFailure("MCP_SCHEMA", "_meta.serverInfo", repr(info))


def _envelope(raw: bytes, reply, error: bool):
    parsed = _load(raw)
    _keys(parsed, ("jsonrpc", "id", "result"), "top")
    if parsed["jsonrpc"] != "2.0":
        raise ParityFailure("MCP_FIELD", "jsonrpc", repr(parsed["jsonrpc"]))
    if parsed["id"] != 1 or isinstance(parsed["id"], bool):
        raise ParityFailure("MCP_FIELD", "id", repr(parsed["id"]))
    if reply != parsed:
        raise ParityFailure("MCP_JSON_MISMATCH", "parsed", "reply differs")
    result = parsed["result"]
    if not isinstance(result, dict):
        raise ParityFailure("MCP_SCHEMA", "result", type(result).__name__)
    if error:
        if result.get("isError") is not True:
            raise ParityFailure("MCP_NOT_ERROR", "isError",
                                repr(result.get("isError")))
        if "structuredContent" in result:
            raise ParityFailure("MCP_STRUCTURED_ON_ERROR", "structuredContent",
                                "success body on error")
        _keys(result, ("isError", "content", "resultType", "_meta"), "result")
    else:
        if result.get("isError") is not False:
            raise ParityFailure("MCP_NOT_SUCCESS", "isError",
                                repr(result.get("isError")))
        _keys(result, ("isError", "structuredContent", "content",
                       "resultType", "_meta"), "result")
    if result["resultType"] != "complete":
        raise ParityFailure("MCP_FIELD", "resultType",
                            repr(result["resultType"]))
    _server_info(result)
    return parsed, result


def _text(result):
    content = result.get("content")
    if not isinstance(content, list) or len(content) != 1:
        raise ParityFailure("MCP_SCHEMA", "content", repr(content))
    item = content[0]
    _keys(item, ("type", "text"), "content[0]")
    if item["type"] != "text":
        raise ParityFailure("MCP_FIELD", "content.type", repr(item["type"]))
    _str(item["text"], "content.text")
    try:
        return json.loads(item["text"], object_pairs_hook=_unique_object)
    except (json.JSONDecodeError, ParityFailure) as error:
        if isinstance(error, ParityFailure):
            raise
        raise ParityFailure("MCP_JSON", "content.text", str(error)) from error


def validate_mcp_v1(raw: bytes, reply, expected: dict):
    if expected.get("mode") != "success":
        raise ParityFailure("MCP_MODE", "mode", repr(expected.get("mode")))
    parsed, result = _envelope(raw, reply, error=False)
    if result["isError"] is not False:
        raise ParityFailure("MCP_NOT_SUCCESS", "isError", repr(result["isError"]))
    structured = result.get("structuredContent")
    keys = ("schema_version", "operation", "source_sha256", "name", "block_id",
            "total", "shown", "skipped", "target_state", "backlinks")
    _keys(structured, keys, "structuredContent")
    _int(structured["schema_version"], "schema_version")
    _str(structured["operation"], "operation")
    _str(structured["source_sha256"], "source_sha256")
    _str(structured["name"], "name")
    _str(structured["block_id"], "block_id")
    _int(structured["total"], "total")
    _int(structured["shown"], "shown")
    _int(structured["skipped"], "skipped")
    _str(structured["target_state"], "target_state")
    if structured["schema_version"] != 1:
        raise ParityFailure("MCP_FIELD", "schema_version",
                            repr(structured["schema_version"]))
    if structured["operation"] != "block_backlinks":
        raise ParityFailure("MCP_FIELD", "operation",
                            repr(structured["operation"]))
    if not _HEX64.fullmatch(structured["source_sha256"]):
        raise ParityFailure("MCP_FIELD", "source_sha256", "not 64 hex")
    for field, want in (("name", expected["name"]),
                        ("block_id", expected["block_id"]),
                        ("total", expected["total"]),
                        ("shown", expected["shown"]),
                        ("skipped", expected["skipped"]),
                        ("target_state", expected["target_state"])):
        if structured[field] != want:
            raise ParityFailure("MCP_FIELD", field,
                                "%r != %r" % (structured[field], want))
    backlinks = structured["backlinks"]
    if not isinstance(backlinks, list) or len(backlinks) != 20:
        raise ParityFailure("MCP_BACKLINKS", "backlinks", repr(backlinks))
    for index, (entry, name) in enumerate(zip(backlinks,
                                              expected["backlinks"])):
        _keys(entry, ("source", "block_id", "line", "start", "end"),
              "backlinks[%d]" % index)
        if entry["source"] != name:
            raise ParityFailure("MCP_BACKLINK_NAME", "backlinks.source",
                                "%r != %r" % (entry["source"], name))
        if entry["block_id"] != expected["block_id"]:
            raise ParityFailure("MCP_BACKLINK_FIELD", "backlinks.block_id",
                                repr(entry["block_id"]))
        for field, want in (("line", expected["line"]),
                            ("start", expected["start"]),
                            ("end", expected["end"])):
            _int(entry[field], "backlinks." + field)
            if entry[field] != want:
                raise ParityFailure("MCP_BACKLINK_FIELD", "backlinks." + field,
                                    "%r != %r" % (entry[field], want))
    text = _text(result)
    if text != structured:
        raise ParityFailure("MCP_TEXT_MISMATCH", "content.text",
                            "text != structuredContent")
    return parsed


def validate_mcp_v2(raw: bytes, reply, expected: dict):
    if expected.get("mode") != "error_parity":
        raise ParityFailure("MCP_MODE", "mode", repr(expected.get("mode")))
    parsed, result = _envelope(raw, reply, error=True)
    if result["isError"] is not True:
        raise ParityFailure("MCP_NOT_ERROR", "isError", repr(result["isError"]))
    if "structuredContent" in result:
        raise ParityFailure("MCP_STRUCTURED_ON_ERROR", "structuredContent",
                            "success body on error")
    text = _text(result)
    _keys(text, ("code",), "error")
    if text["code"] != expected["code"]:
        raise ParityFailure("MCP_ERROR_CODE", "code", repr(text["code"]))
    return parsed


def compare_mcp(left: McpObservation, right: McpObservation, expected: dict,
                expected_image: bytes):
    if not left.raw or not right.raw:
        raise ParityFailure("MCP_EMPTY", "raw", "one side empty")
    if left.raw != right.raw:
        raise ParityFailure("MCP_WIRE_MISMATCH", "wire",
                            "first difference at %d" % _first_diff(
                                left.raw, right.raw))
    if left.exit != 0 or right.exit != 0:
        raise ParityFailure("MCP_EXIT", "exit",
                            "%r/%r" % (left.exit, right.exit))
    if left.image_before != expected_image or right.image_before != expected_image:
        raise ParityFailure("MCP_IMAGE_BEFORE", "image_before", "input changed")
    if left.image_after != expected_image or right.image_after != expected_image:
        raise ParityFailure("MCP_IMAGE", "image_sha256", "MCP changed the image")
    if expected["mode"] == "success":
        a = validate_mcp_v1(left.raw, left.reply, expected)
        b = validate_mcp_v1(right.raw, right.reply, expected)
    else:
        a = validate_mcp_v2(left.raw, left.reply, expected)
        b = validate_mcp_v2(right.raw, right.reply, expected)
    if a != b:
        raise ParityFailure("MCP_JSON_MISMATCH", "parsed", "replies differ")
    return a


# ------------------------------------------------------------ named inputs

def check_fresh_work(work, source):
    """Ownership predicate only: never creates, writes or cleans anything."""
    work_path = Path(work).resolve()
    source_path = Path(source).resolve()
    if work_path == source_path or work_path in source_path.parents:
        raise ParityFailure("WORK_NOT_FRESH", "work",
                            "refusing the repository root itself")
    if work_path.exists():
        if not work_path.is_dir():
            raise ParityFailure("WORK_NOT_FRESH", "work", "not a directory")
        if any(work_path.iterdir()):
            raise ParityFailure("WORK_NOT_FRESH", "work",
                                "directory is not empty")
    return work_path


def require_named_inputs(root, paths):
    root = Path(root)
    for relative in paths:
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts:
            raise ParityFailure("INPUT_PATH", "inputs", str(relative))
        target = root / path
        if not target.is_file():
            raise ParityFailure("MISSING_INPUT", "inputs", str(relative))
    return True


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha256(path) -> str:
    with open(path, "rb") as stream:
        return hashlib.sha256(stream.read()).hexdigest()


__all__ = [
    "BLOCK", "CAPTURE_CAPACITY", "CORRUPT_CAPTURE", "CORRUPT_TOHOST",
    "CORRUPT_CASES", "McpObservation", "MCP_CASE", "NORMAL_CASES", "NOTE",
    "PLAIN", "ParityFailure", "REF_BASELINE", "REF_CANDIDATE", "SIZES",
    "TRAILER", "TREE_BASELINE", "TREE_CANDIDATE", "TerminalObservation",
    "assert_case_matrix", "case_matrix", "check_corrupt_observation",
    "check_extents", "check_fresh_work", "check_terminal_observation",
    "check_unique_names",
    "compare_corrupt", "compare_mcp", "compare_terminal",
    "corrupt_duplicate", "corrupt_overlap", "decode", "directory_blocks",
    "encode", "entry_location", "expected_corrupt", "expected_mcp",
    "expected_terminal", "file_sha256", "plain_name", "plain_slot",
    "recompute_crc", "require_named_inputs", "sha256_hex", "slot_names",
    "validate_fixture", "validate_mcp_v1", "validate_mcp_v2",
]
