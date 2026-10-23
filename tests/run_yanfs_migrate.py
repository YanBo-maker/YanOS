#!/usr/bin/env python3
"""0036-R7 independent acceptance harness for the offline image migration.

This file deliberately does not import or link any production decoder.  It
implements the v1/v2 directory format, the IEEE reflected CRC32, the 0033
source_sha256 identity and the `.assets` tree walk from the published specs,
builds its own sparse images, and then drives the real candidate CLI/library
against them.

Required artifacts (all are hard failures when missing; only a missing external
compiler can produce exit 77):

    --migrate       real yan_migrate candidate
    --terms-probe   tests/yanfs_migrate_terms_probe.c built against the snapshot
    --fault         tests/yanfs_migrate_fault.c built with ld --wrap against the
                    snapshot

Optional, never counted unless usable:

    --semantic --model --runner --index

Exit codes:
    0  every counted case passed
    1  at least one counted case failed, or a required artifact is missing
    77 only when an external compiler is required by an optional build step and
       is unavailable

Run with no network access and no production-source writes.
"""
from __future__ import annotations

import argparse
import errno
import fcntl
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path

BLOCK = 4096
MAGIC = b"YANFS01\x00"
NAME_BYTES = 32
ENTRY_BYTES = 64
HEADER_BYTES = 64
CRC_OFFSET = 60
ALLOWED_NAME = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")
READY = 1
SCAN = 1


class OracleError(Exception):
    pass


class CaseFailure(Exception):
    pass


class CaseSkip(Exception):
    pass


class CompilerMissing(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CaseFailure(message)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def p32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def crc32_ieee(data: bytes) -> int:
    """IEEE reflected CRC32, polynomial 0xedb88320, initial/final xor."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass
class Entry:
    slot: int
    name: str
    size: int
    start: int
    count: int
    data: bytes


@dataclass
class Image:
    path: Path
    raw: bytes
    version: int
    directory_blocks: int
    capacity: int
    entry_capacity: int
    entries: list[Entry] = field(default_factory=list)

    @property
    def by_slot(self) -> dict[int, Entry]:
        return {entry.slot: entry for entry in self.entries}

    @property
    def live(self) -> list[Entry]:
        return list(self.entries)


def _decode_name(entry: bytes) -> str:
    area = entry[:NAME_BYTES]
    end = area.find(b"\x00")
    if end < 0:
        raise OracleError("entry name has no terminator inside 32 bytes")
    name = area[:end]
    if len(name) == 0:
        if any(entry):
            raise OracleError("empty first byte but non-zero entry")
        return ""
    if len(name) > 31 or not set(name) <= ALLOWED_NAME:
        raise OracleError(f"illegal file name {name!r}")
    if name in (b".", b".."):
        raise OracleError(f"illegal file name {name!r}")
    if any(area[end:]):
        raise OracleError(f"name padding after terminator is not zero: {name!r}")
    return name.decode("ascii")


def decode_image(path: Path) -> Image:
    raw = Path(path).read_bytes()
    if len(raw) < BLOCK or len(raw) % BLOCK != 0:
        raise OracleError(f"image size {len(raw)} is not a positive multiple of 4096")
    capacity = len(raw) // BLOCK
    if raw[0:8] != MAGIC:
        raise OracleError("bad magic")
    version = u32(raw, 8)
    block_size = u32(raw, 12)
    capacity_field = u32(raw, 16)
    entry_capacity = u32(raw, 20)
    if version == 1:
        directory_blocks = 1
        if entry_capacity != 63:
            raise OracleError("v1 entry_capacity is not 63")
        if any(raw[24:60]):
            raise OracleError("v1 reserved field is not zero")
    elif version == 2:
        directory_blocks = u32(raw, 24)
        if not 1 <= directory_blocks <= 64:
            raise OracleError(f"v2 directory_blocks {directory_blocks} out of range")
        if entry_capacity != directory_blocks * 64 - 1:
            raise OracleError("v2 entry_capacity != 64*D-1")
        if u32(raw, 24) != directory_blocks or any(raw[28:60]):
            raise OracleError("v2 reserved field is not exactly D at offset 24")
    else:
        raise OracleError(f"unsupported version {version}")
    if block_size != BLOCK:
        raise OracleError(f"block_size {block_size} != 4096")
    if capacity_field != capacity:
        raise OracleError(f"capacity field {capacity_field} != file blocks {capacity}")
    if directory_blocks > capacity or (directory_blocks > 1 and directory_blocks == capacity):
        raise OracleError("directory blocks do not fit the image")
    directory = bytearray(raw[: directory_blocks * BLOCK])
    stored = u32(directory, CRC_OFFSET)
    directory[CRC_OFFSET:CRC_OFFSET + 4] = b"\x00" * 4
    actual = crc32_ieee(bytes(directory))
    if stored != actual:
        raise OracleError(f"directory CRC {stored:#08x} != {actual:#08x}")

    entries: list[Entry] = []
    for slot in range(entry_capacity):
        offset = HEADER_BYTES + slot * ENTRY_BYTES
        entry = bytes(raw[offset:offset + ENTRY_BYTES])
        if len(entry) != ENTRY_BYTES:
            raise OracleError(f"slot {slot} is outside the image")
        name = _decode_name(entry)
        size = u32(entry, 32)
        start = u32(entry, 36)
        count = u32(entry, 40)
        if any(entry[44:64]):
            raise OracleError(f"slot {slot} entry reserved bytes are not zero")
        if name == "":
            continue
        if size == 0:
            if start != 0 or count != 0:
                raise OracleError(f"empty file {name} owns an extent")
            data = b""
        else:
            needed = (size + BLOCK - 1) // BLOCK
            if count != needed:
                raise OracleError(f"{name}: count {count} != needed {needed}")
            if start < directory_blocks or start >= capacity:
                raise OracleError(f"{name}: start {start} outside data area")
            if count > capacity - start:
                raise OracleError(f"{name}: extent runs past capacity")
            begin = start * BLOCK
            data = bytes(raw[begin:begin + size])
            if len(data) != size:
                raise OracleError(f"{name}: truncated payload")
        entries.append(Entry(slot, name, size, start, count, data))
    ordered = sorted((e for e in entries if e.size), key=lambda e: e.start)
    for previous, current in zip(ordered, ordered[1:]):
        if previous.start + previous.count > current.start:
            raise OracleError(f"extents overlap: {previous.name}/{current.name}")
    return Image(Path(path), raw, version, directory_blocks, capacity,
                 entry_capacity, entries)


def check_compact(image: Image) -> list[str]:
    errors: list[str] = []
    expected = image.directory_blocks
    for entry in sorted((e for e in image.live if e.size), key=lambda e: e.start):
        if entry.start != expected:
            errors.append(f"{entry.name}: start {entry.start} != compact {expected}")
        expected += entry.count
    return errors


def check_tail_padding(image: Image) -> list[str]:
    errors: list[str] = []
    for entry in image.live:
        if not entry.size:
            continue
        begin = (entry.start * BLOCK) + entry.size
        end = (entry.start + entry.count) * BLOCK
        if any(image.raw[begin:end]):
            errors.append(f"{entry.name}: bytes after logical size are not zero")
    return errors


def source_sha256(image: Image) -> str:
    """0033/0030 identity: ASCII-name order, slot/name-len/name/size/raw bytes."""
    digest = hashlib.sha256()
    for entry in sorted(image.live, key=lambda e: e.name.encode("ascii")):
        name = entry.name.encode("ascii")
        digest.update(p32(entry.slot))
        digest.update(p32(len(name)))
        digest.update(name)
        digest.update(p32(entry.size))
        digest.update(entry.data)
    return digest.hexdigest()


def build_image(path: Path, capacity: int, directory_blocks: int,
                files: dict[int, tuple[str, bytes]]) -> Image:
    if directory_blocks < 1 or directory_blocks > 64:
        raise OracleError("directory_blocks out of range")
    if directory_blocks > 1 and capacity <= directory_blocks:
        raise OracleError("v2 needs at least one data block")
    if directory_blocks == 1 and capacity < 1:
        raise OracleError("capacity must be positive")
    entry_capacity = directory_blocks * 64 - 1
    version = 1 if directory_blocks == 1 else 2
    for slot, (name, _) in files.items():
        if not 0 <= slot < entry_capacity:
            raise OracleError(f"slot {slot} outside capacity {entry_capacity}")
        if not name or len(name) > 31 or not set(name.encode("ascii")) <= ALLOWED_NAME:
            raise OracleError(f"fixture name {name!r} is not a legal YanFS name")
    buffer = bytearray(capacity * BLOCK)
    buffer[0:8] = MAGIC
    struct.pack_into("<I", buffer, 8, version)
    struct.pack_into("<I", buffer, 12, BLOCK)
    struct.pack_into("<I", buffer, 16, capacity)
    struct.pack_into("<I", buffer, 20, entry_capacity)
    struct.pack_into("<I", buffer, 24, directory_blocks if version == 2 else 0)
    next_block = directory_blocks
    for slot in sorted(files):
        name, content = files[slot]
        encoded = name.encode("ascii")
        offset = HEADER_BYTES + slot * ENTRY_BYTES
        buffer[offset:offset + len(encoded)] = encoded
        struct.pack_into("<I", buffer, offset + 32, len(content))
        if content:
            count = (len(content) + BLOCK - 1) // BLOCK
            if next_block + count > capacity:
                raise OracleError("fixture does not fit")
            struct.pack_into("<I", buffer, offset + 36, next_block)
            struct.pack_into("<I", buffer, offset + 40, count)
            begin = next_block * BLOCK
            buffer[begin:begin + len(content)] = content
            next_block += count
    directory = bytearray(buffer[: directory_blocks * BLOCK])
    directory[CRC_OFFSET:CRC_OFFSET + 4] = b"\x00" * 4
    struct.pack_into("<I", buffer, CRC_OFFSET, crc32_ieee(bytes(directory)))
    Path(path).write_bytes(buffer)
    return decode_image(path)


def assets_path(image: Path) -> Path:
    return Path(os.path.realpath(str(image)) + ".assets")


def asset_manifest(root: Path) -> list[tuple] | None:
    """Structure plus per-file bytes, symlinks recorded without following."""
    if not os.path.lexists(root):
        return None
    root = Path(root)
    info = os.lstat(root)
    if stat.S_ISLNK(info.st_mode):
        return [(".", "symlink", os.readlink(root), 0, "")]
    if not stat.S_ISDIR(info.st_mode):
        return [(".", "file", "", info.st_size, sha256_file(root))]
    records: list[tuple] = []

    def walk(directory: Path, relative: str) -> None:
        for item in sorted(os.scandir(directory), key=lambda e: e.name):
            rel = relative + item.name
            item_info = os.lstat(item.path)
            if stat.S_ISLNK(item_info.st_mode):
                records.append((rel, "symlink", os.readlink(item.path), 0, ""))
            elif stat.S_ISDIR(item_info.st_mode):
                records.append((rel, "dir", "", 0, ""))
                walk(Path(item.path), rel + "/")
            elif stat.S_ISREG(item_info.st_mode):
                records.append((rel, "file", "", item_info.st_size,
                                sha256_file(Path(item.path))))
            else:
                records.append((rel, "special", oct(stat.S_IFMT(item_info.st_mode)),
                                0, ""))
    walk(root, "")
    return records


def asset_hash(root: Path) -> str:
    manifest = asset_manifest(root)
    return sha256_bytes(json.dumps(manifest, sort_keys=True).encode("utf-8"))


def make_asset_fixture(image: Path) -> None:
    root = assets_path(image)
    (root / "img" / "deep").mkdir(parents=True)
    (root / "empty").mkdir()
    (root / "img" / "x.png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + bytes(range(64)) + b"R7-ASSET")
    blob = bytes(((i * 37) + 11) & 0xFF for i in range(5000))
    (root / "img" / "deep" / "blob.bin").write_bytes(blob)


def clone_image(src: Path, dst: Path) -> None:
    shutil.copyfile(src, dst)
    source_assets = assets_path(src)
    if os.path.lexists(source_assets):
        destination = assets_path(dst)
        if os.path.lexists(destination):
            shutil.rmtree(destination, ignore_errors=True)
        shutil.copytree(source_assets, destination, symlinks=True)


class Harness:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.migrate = Path(args.migrate).resolve()
        self.terms = Path(args.terms_probe).resolve()
        self.fault = Path(args.fault).resolve()
        self.work = Path(args.work).resolve()
        self.results: list[tuple[str, str, str, str]] = []
        self.failures = 0
        self.passes = 0
        self.skips = 0
        self.exit_code = 0

    # ---- process / assertion helpers ----
    def run(self, command: list[str], timeout: int = 240,
            env: dict | None = None) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(
                command, cwd=str(self.work), env=env, text=True,
                capture_output=True, timeout=timeout)
        except FileNotFoundError as error:
            raise CaseFailure(f"cannot execute {command[0]}: {error}") from error
        except subprocess.TimeoutExpired as error:
            raise CaseFailure(f"timeout after {timeout}s: {' '.join(command)}") from error

    def record(self, name: str, ac: str, status: str, detail: str = "") -> None:
        self.results.append((name, ac, status, detail))
        suffix = f" {detail}" if detail else ""
        print(f"[{status}] {name} [{ac}]{suffix}", flush=True)
        if status == "PASS":
            self.passes += 1
        elif status == "FAIL":
            self.failures += 1
        else:
            self.skips += 1

    def case(self, name: str, ac: str, function) -> None:
        try:
            function()
        except CaseSkip as error:
            self.record(name, ac, "SKIP", str(error))
        except CaseFailure as error:
            self.record(name, ac, "FAIL", str(error))
        except Exception as error:  # noqa: BLE001 - report, never hide
            detail = f"{type(error).__name__}: {error}"
            self.record(name, ac, "FAIL", detail)
            if self.args.trace:
                traceback.print_exc()
        else:
            self.record(name, ac, "PASS")

    def cmd(self, old: Path, new: Path, files: int,
            blocks: int | None = None) -> list[str]:
        command = [str(self.migrate), "--from", str(old), "--to", str(new),
                   "--files", str(files)]
        if blocks is not None:
            command += ["--blocks", str(blocks)]
        return command

    @staticmethod
    def assert_absent(path: Path, label: str = "") -> None:
        if os.path.lexists(path):
            raise CaseFailure(f"unexpected residual path exists: {label or path}")

    @staticmethod
    def assert_unchanged(path: Path, digest: str, label: str) -> None:
        if not os.path.lexists(path):
            raise CaseFailure(f"{label} disappeared")
        require(sha256_file(path) == digest, f"{label} bytes changed")

    def compare_images(self, old: Image, new: Image, files: int,
                       capacity: int) -> None:
        directory_blocks = (files + 1) // 64
        errors: list[str] = []
        if new.version != (1 if directory_blocks == 1 else 2):
            errors.append(f"version {new.version} != {1 if directory_blocks == 1 else 2}")
        if new.directory_blocks != directory_blocks:
            errors.append(f"D {new.directory_blocks} != {directory_blocks}")
        if new.entry_capacity != files:
            errors.append(f"entry_capacity {new.entry_capacity} != {files}")
        if new.capacity != capacity:
            errors.append(f"capacity {new.capacity} != {capacity}")
        if len(new.raw) != capacity * BLOCK:
            errors.append(f"file size {len(new.raw)} != {capacity * BLOCK}")
        old_slots = {entry.slot for entry in old.live}
        new_slots = {entry.slot for entry in new.live}
        if old_slots != new_slots:
            errors.append(f"live slot set differs: {sorted(old_slots)} vs {sorted(new_slots)}")
        old_by_slot = old.by_slot
        new_by_slot = new.by_slot
        for slot in sorted(old_slots & new_slots):
            left, right = old_by_slot[slot], new_by_slot[slot]
            if left.name != right.name:
                errors.append(f"slot {slot} name {right.name!r} != {left.name!r}")
            if left.size != right.size:
                errors.append(f"slot {slot} size {right.size} != {left.size}")
            if left.data != right.data:
                errors.append(f"slot {slot} ({left.name}) bytes differ")
        errors.extend(check_compact(new))
        errors.extend(check_tail_padding(new))
        if source_sha256(old) != source_sha256(new):
            errors.append("source_sha256 differs")
        if errors:
            raise CaseFailure("; ".join(errors[:8]))

    def migrate_ok(self, old: Path, new: Path, files: int,
                   blocks: int | None = None) -> Image:
        old_hash = sha256_file(old)
        old_assets = asset_manifest(assets_path(old))
        old_image = decode_image(old)
        directory_blocks = (files + 1) // 64
        expected_capacity = (blocks if blocks is not None else
                             old_image.capacity +
                             (directory_blocks - old_image.directory_blocks))
        proc = self.run(self.cmd(old, new, files, blocks))
        require(proc.returncode == 0,
                f"exit {proc.returncode}; stderr={proc.stderr.strip()[:500]}")
        require(proc.stdout.startswith("yan_migrate: migrated "),
                f"success stdout missing or wrong: {proc.stdout!r}")
        require(proc.stderr == "",
                f"success wrote stderr: {proc.stderr!r}")
        new_image = decode_image(new)
        self.compare_images(old_image, new_image, files, expected_capacity)
        if old_assets is not None:
            require(asset_manifest(assets_path(new)) == old_assets,
                    "NEW.assets does not match OLD.assets")
        else:
            self.assert_absent(assets_path(new), "NEW.assets")
        self.assert_unchanged(old, old_hash, "OLD")
        require(asset_manifest(assets_path(old)) == old_assets,
                "OLD.assets changed")
        require(os.path.lexists(Path(os.path.realpath(str(old)) + ".lock")),
                "source .lock missing after migration")
        return new_image

    def migrate_fail(self, old: Path, new: Path, files: int,
                     blocks: int | None = None,
                     no_residue: bool = True,
                     expected_exit: int | None = None) -> subprocess.CompletedProcess:
        old_hash = sha256_file(old)
        old_assets = asset_manifest(assets_path(old))
        proc = self.run(self.cmd(old, new, files, blocks))
        require(proc.returncode != 0, "expected a nonzero exit, got 0")
        if expected_exit is not None:
            require(proc.returncode == expected_exit,
                    f"exit {proc.returncode} != stable expected {expected_exit}")
        require(proc.stdout == "",
                f"failure printed a success-looking stdout: {proc.stdout!r}")
        require(bool(proc.stderr.strip()), "failure produced no diagnostic")
        self.assert_unchanged(old, old_hash, "OLD")
        require(asset_manifest(assets_path(old)) == old_assets,
                "OLD.assets changed")
        require(os.path.lexists(Path(os.path.realpath(str(old)) + ".lock")),
                "source .lock missing after failed migration")
        if no_residue:
            self.assert_absent(new, "NEW")
            self.assert_absent(assets_path(new), "NEW.assets")
        return proc


FILES_V1 = {
    0: ("alpha.txt", b"shared alpha line\nshared second line\n"),
    2: ("note.txt",
        "标题: 迁移验收\nshared 笔记内容 zebrafish\n\n"
        "[[alpha.txt#intro]]\nlink: alpha.txt\n#intro\n标签: 测试 标签\n".encode("utf-8")),
    5: ("empty.txt", b""),
    7: ("blob.bin", bytes(((i * 7) + 3) & 0xFF for i in range(9000)) +
        b"\x00\x01\x02\xa5\xfe"),
    62: ("omega.txt", b"omega shared tail\n"),
}


def prepare_fixtures(work: Path) -> dict[str, Path]:
    fixtures = work / "yanfs_migrate_fixtures"
    fixtures.mkdir(parents=True)
    paths: dict[str, Path] = {}

    v1 = fixtures / "yanfs_migrate_v1_sparse.img"
    build_image(v1, 256, 1, FILES_V1)
    make_asset_fixture(v1)
    paths["v1"] = v1

    empty = fixtures / "yanfs_migrate_v1_empty.img"
    build_image(empty, 16, 1, {})
    paths["v1_empty"] = empty

    minimum = fixtures / "yanfs_migrate_v1_one_block.img"
    build_image(minimum, 1, 1, {})
    paths["v1_minimum"] = minimum

    v2_high = fixtures / "yanfs_migrate_v2_high.img"
    build_image(v2_high, 64, 16, {
        5: ("base.txt", b"base shared\n"),
        1000: ("high.txt", b"high slot shared zebrafish\n"),
    })
    paths["v2_high"] = v2_high

    v2_empty = fixtures / "yanfs_migrate_v2_empty.img"
    build_image(v2_empty, 32, 8, {})
    paths["v2_empty"] = v2_empty

    corrupt = fixtures / "yanfs_migrate_corrupt.img"
    shutil.copyfile(v1, corrupt)
    raw = bytearray(corrupt.read_bytes())
    raw[CRC_OFFSET] ^= 0xFF
    corrupt.write_bytes(raw)
    paths["corrupt"] = corrupt

    fault_old = fixtures / "yanfs_migrate_fault_old.img"
    clone_image(v1, fault_old)
    paths["fault_old"] = fault_old

    alias_old = fixtures / "yanfs_migrate_alias_old.img"
    build_image(alias_old, 16, 1, {0: ("alias.txt", b"alias data\n")})
    paths["alias_old"] = alias_old

    assets_file = fixtures / "yanfs_migrate_assets_file.img"
    build_image(assets_file, 16, 1, {})
    Path(str(assets_file) + ".assets").write_bytes(b"not a directory\n")
    paths["assets_file"] = assets_file

    assets_link = fixtures / "yanfs_migrate_assets_link.img"
    build_image(assets_link, 16, 1, {})
    target = fixtures / "yanfs_migrate_assets_link_target"
    target.mkdir()
    (target / "sentinel.txt").write_bytes(b"sentinel\n")
    os.symlink(target, str(assets_link) + ".assets")
    paths["assets_link"] = assets_link
    paths["assets_link_target"] = target

    child_link = fixtures / "yanfs_migrate_assets_child_link.img"
    clone_image(v1, child_link)
    os.symlink("/tmp", str(assets_path(child_link) / "evil-link"))
    paths["assets_child_link"] = child_link

    child_fifo = fixtures / "yanfs_migrate_assets_child_fifo.img"
    clone_image(v1, child_fifo)
    try:
        os.mkfifo(str(assets_path(child_fifo) / "evil-fifo"))
    except OSError as error:
        # DrvFs (this WSL checkout) and some CI mounts reject mkfifo.  Rebuild
        # this one fixture on a POSIX temporary filesystem instead of silently
        # dropping the special-file case.  The migration itself still runs on
        # the real candidate and the real image layout.
        shutil.rmtree(assets_path(child_fifo), ignore_errors=True)
        temp_root = None
        for candidate in ("/dev/shm", "/tmp"):
            if os.path.isdir(candidate):
                try:
                    temp_root = Path(tempfile.mkdtemp(
                        prefix="yanfs_migrate_fifo_", dir=candidate))
                    break
                except OSError:
                    continue
        if temp_root is None:
            raise CaseSkip("host filesystem cannot create a FIFO fixture") from error
        paths.setdefault("_temp_dirs", []).append(temp_root)
        child_fifo = temp_root / "yanfs_migrate_assets_child_fifo.img"
        clone_image(v1, child_fifo)
        os.mkfifo(str(assets_path(child_fifo) / "evil-fifo"))
        paths["assets_child_fifo_fallback"] = temp_root
    paths["assets_child_fifo"] = child_fifo
    return paths


PRODUCTION_COMMON = (
    "os/yanfs.c",
    "host/note_write.c",
    "host/semantic_posix.c",
    "host/semantic.c",
    "host/semantic_corpus.c",
    "host/semantic_sha256.c",
    "host/semantic_text.c",
    "os/note_metadata_format.c",
    "os/note_blocks_format.c",
    "os/search_text.c",
)


def require_compiler(cc: str) -> str:
    found = shutil.which(cc)
    if found is None:
        raise CompilerMissing(f"external compiler not found: {cc}")
    return found


def compile_test_binary(cc: str, root: Path, output: Path,
                        sources: list[Path], wrap: list[str]) -> Path:
    compiler = require_compiler(cc)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [compiler, "-I", str(root / "host"), "-I", str(root / "os"),
               "-I", str(root / "tools"), "-std=c17",
               "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
               "-Wpedantic", "-Werror", "-o", str(output)]
    command.extend(str(source) for source in sources)
    command.extend(f"-Wl,--wrap={symbol}" for symbol in wrap)
    command.append("-lm")
    proc = subprocess.run(command, text=True, capture_output=True)
    if proc.returncode != 0:
        raise CaseFailure(
            f"compile failed ({proc.returncode}): {' '.join(command)}\n"
            f"{proc.stderr[-1200:]}")
    return output


def build_bad_candidate(cc: str, root: Path, output_dir: Path,
                        kind: str) -> Path:
    """Compile a one-byte-corrupt candidate from the current checkout."""
    source = (root / "host" / "image_migrate.c").read_text()
    if kind == "image":
        old1 = """    uint8_t buffer[YAN_FS_BLOCK_SIZE];
    uint32_t entries = directory_blocks * 64u - 1u;"""
        new1 = """    uint8_t buffer[YAN_FS_BLOCK_SIZE];
    bool corrupt_done = false;
    uint32_t entries = directory_blocks * 64u - 1u;"""
        old2 = """            if (!pwrite_exact(target, buffer, YAN_FS_BLOCK_SIZE,
                              ((uint64_t)start + index) * YAN_FS_BLOCK_SIZE)) {"""
        new2 = """            if (!corrupt_done && slot == 0u && index == 0u) {
                corrupt_done = true;
                buffer[0] ^= 0x01u;
            }
            if (!pwrite_exact(target, buffer, YAN_FS_BLOCK_SIZE,
                              ((uint64_t)start + index) * YAN_FS_BLOCK_SIZE)) {"""
        old3 = """                old_got != want || new_got != want ||
                memcmp(old_buffer, new_buffer, want) != 0) {"""
        new3 = """                old_got != want || new_got != want ||
                0) {"""
        old4 = """    if (memcmp(old_digest, new_digest, sizeof old_digest) != 0) {"""
        new4 = """    if (0) {"""
        for old, new in ((old1, new1), (old2, new2), (old3, new3),
                         (old4, new4)):
            if old not in source:
                raise CaseFailure(
                    f"bad image patch pattern missing ({kind}); source changed")
            source = source.replace(old, new)
    elif kind == "assets":
        old1 = """    uint8_t left[MIG_COPY_BUFFER];
    uint8_t right[MIG_COPY_BUFFER];
    if (mode == ASSETS_COPY) {"""
        new1 = """    uint8_t left[MIG_COPY_BUFFER];
    uint8_t right[MIG_COPY_BUFFER];
    bool corrupt_asset_done = false;
    if (mode == ASSETS_COPY) {"""
        old2 = """            if (!write_exact(out, left, (size_t)got)) {"""
        new2 = """            if (!corrupt_asset_done && got > 0) {
                corrupt_asset_done = true;
                left[0] ^= 0x01u;
            }
            if (!write_exact(out, left, (size_t)got)) {"""
        old3 = """        if (!read_exact(in, left, chunk) || !read_exact(out, right, chunk) ||
            memcmp(left, right, chunk) != 0) {"""
        new3 = """        if (!read_exact(in, left, chunk) || !read_exact(out, right, chunk) ||
            0) {"""
        for old, new in ((old1, new1), (old2, new2), (old3, new3)):
            if old not in source:
                raise CaseFailure(
                    f"bad assets patch pattern missing ({kind}); source changed")
            source = source.replace(old, new)
    else:
        raise CaseFailure(f"unknown bad candidate kind {kind}")

    patched = output_dir / f"{kind}-image_migrate.c"
    patched.parent.mkdir(parents=True, exist_ok=True)
    patched.write_text(source)
    sources = [patched, root / "tools" / "yan_migrate.c"]
    sources.extend(root / name for name in PRODUCTION_COMMON)
    return compile_test_binary(cc, root, output_dir / f"bad-{kind}" / "yan_migrate",
                               sources, [])


def build_cli_fault(cc: str, root: Path, output: Path) -> Path:
    sources = [root / "tests" / "yanfs_migrate_cli_fault.c",
               root / "tools" / "yan_migrate.c",
               root / "host" / "image_migrate.c"]
    sources.extend(root / name for name in PRODUCTION_COMMON)
    return compile_test_binary(cc, root, output, sources,
                               ["open", "close", "unlinkat"])


def run_terms_probe(harness: Harness, image: Path, queries: list[str]) -> str:
    proc = harness.run([str(harness.terms), str(image)] + queries)
    require(proc.returncode == 0,
            f"terms probe exit {proc.returncode}; stderr={proc.stderr.strip()[:300]}")
    return proc.stdout


def add_ac1_cases(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_out"
    out.mkdir(exist_ok=True)

    h.case("AC1.v1-one-block-default-rejected", "0036-R7-AC1",
           lambda: h.migrate_fail(f["v1_minimum"], out / "minimum-default.img",
                                  1023, expected_exit=5))
    h.case("AC1.v1-one-block-explicit-17", "0036-R7-AC1",
           lambda: h.migrate_ok(f["v1_minimum"], out / "minimum-explicit.img",
                                1023, blocks=17))

    def default_v1():
        h.migrate_ok(f["v1"], out / "ac1-default-v1.img", 1023)
    h.case("AC1.v1-default-files1023-assets", "0036-R7-AC1", default_v1)

    def explicit_blocks():
        h.migrate_ok(f["v1"], out / "ac1-explicit-128.img", 1023, 128)
    h.case("AC1.v1-explicit-blocks-128", "0036-R7-AC1", explicit_blocks)

    def v2_expand():
        image = h.migrate_ok(f["v2_high"], out / "ac1-v2-expand.img", 2047)
        require(image.directory_blocks == 32, "expansion did not select D=32")
        require((out / "ac1-v2-expand.img").stat().st_size == 80 * BLOCK,
                "v2 default block formula is not 64+(32-16)=80")
    h.case("AC1.v2-expand-high-slot-2047", "0036-R7-AC1", v2_expand)

    def v2_shrink():
        image = h.migrate_ok(f["v2_high"], out / "ac1-v2-shrink.img", 1023, 64)
        require(image.directory_blocks == 16, "shrink did not select D=16")
    h.case("AC1.v2-shrink-feasible-1023", "0036-R7-AC1", v2_shrink)

    def empty_v1():
        image = h.migrate_ok(f["v1_empty"], out / "ac1-empty-v1.img", 1023)
        require(not image.live, "empty source produced live slots")
        require((out / "ac1-empty-v1.img").stat().st_size == 31 * BLOCK,
                "v1 empty default formula is not 16+(16-1)=31")
    h.case("AC1.v1-empty-default-formula", "0036-R7-AC1", empty_v1)

    def empty_4095():
        image = h.migrate_ok(f["v1_empty"], out / "ac1-empty-4095.img", 4095)
        require(image.entry_capacity == 4095 and image.directory_blocks == 64,
                "4095 boundary did not produce D=64/N=4095")
        require((out / "ac1-empty-4095.img").stat().st_size == 79 * BLOCK,
                "4095 default formula is not 16+(64-1)=79")
    h.case("AC1.v1-empty-files4095-boundary", "0036-R7-AC1", empty_4095)

    def empty_v2():
        image = h.migrate_ok(f["v2_empty"], out / "ac1-empty-v2.img", 1023)
        require(image.directory_blocks == 16, "v2 empty did not expand to D=16")
        require((out / "ac1-empty-v2.img").stat().st_size == 40 * BLOCK,
                "v2 empty default formula is not 32+(16-8)=40")
    h.case("AC1.v2-empty-default-formula", "0036-R7-AC1", empty_v2)

    def n_too_small():
        h.migrate_fail(f["v2_high"], out / "ac1-v2-too-small.img", 511, expected_exit=5)
    h.case("AC1.v2-files-below-highest-slot", "0036-R7-AC1", n_too_small)

    def files63_v1():
        image = h.migrate_ok(f["v1"], out / "ac1-v1-files63.img", 63)
        require(image.version == 1 and image.directory_blocks == 1,
                "--files 63 did not produce a v1/D=1 image")
        require((out / "ac1-v1-files63.img").stat().st_size == 256 * BLOCK,
                "v1 --files 63 default formula is not 256+(1-1)=256")
    h.case("AC1.v1-files63-stays-v1", "0036-R7-AC1", files63_v1)


def add_ac2_cases(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_out"
    out.mkdir(exist_ok=True)

    def new_regular_exists():
        new = out / "ac2-existing-file.img"
        new.write_bytes(b"sentinel-new-bytes")
        before = new.read_bytes()
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False, expected_exit=3)
        require("EXISTS" in proc.stderr, "existing regular file was not reported EXISTS")
        require(new.read_bytes() == before, "existing regular file was changed")
        h.assert_absent(assets_path(new), "NEW.assets")
    h.case("AC2.new-regular-file-exists", "0036-R7-AC2", new_regular_exists)

    def new_directory_exists():
        new = out / "ac2-existing-dir.img"
        new.mkdir()
        (new / "sentinel").write_bytes(b"keep\n")
        manifest = asset_manifest(new)
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False, expected_exit=3)
        require("EXISTS" in proc.stderr, "existing directory was not reported EXISTS")
        require(asset_manifest(new) == manifest, "existing directory was changed")
        h.assert_absent(assets_path(new), "NEW.assets")
    h.case("AC2.new-directory-exists", "0036-R7-AC2", new_directory_exists)

    def new_dangling_symlink():
        new = out / "ac2-dangling.img"
        os.symlink("does-not-exist-target", new)
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False, expected_exit=3)
        require("EXISTS" in proc.stderr, "dangling symlink was not reported EXISTS")
        require(os.path.islink(new), "dangling symlink was replaced")
        require(os.readlink(new) == "does-not-exist-target", "dangling link target changed")
        h.assert_absent(assets_path(new), "NEW.assets")
    h.case("AC2.new-dangling-symlink", "0036-R7-AC2", new_dangling_symlink)

    def new_assets_directory_exists():
        new = out / "ac2-existing-assets.img"
        assets = assets_path(new)
        assets.mkdir()
        (assets / "sentinel").write_bytes(b"keep-assets\n")
        manifest = asset_manifest(assets)
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False, expected_exit=3)
        require("EXISTS" in proc.stderr, "existing NEW.assets was not reported EXISTS")
        h.assert_absent(new, "NEW")
        require(asset_manifest(assets) == manifest, "existing NEW.assets was changed")
    h.case("AC2.new-assets-directory-exists", "0036-R7-AC2", new_assets_directory_exists)

    def new_assets_symlink_exists():
        new = out / "ac2-existing-assets-link.img"
        target = out / "ac2-assets-link-target"
        target.mkdir()
        (target / "sentinel").write_bytes(b"keep-link\n")
        os.symlink(target, assets_path(new))
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False, expected_exit=3)
        require("EXISTS" in proc.stderr, "symlink NEW.assets was not reported EXISTS")
        h.assert_absent(new, "NEW")
        require(os.path.islink(assets_path(new)), "NEW.assets symlink was replaced")
        require((target / "sentinel").read_bytes() == b"keep-link\n",
                "NEW.assets symlink target changed")
    h.case("AC2.new-assets-symlink-exists", "0036-R7-AC2", new_assets_symlink_exists)

    def new_assets_regular_file_exists():
        new = out / "ac2-existing-assets-file.img"
        sentinel = assets_path(new)
        sentinel.write_bytes(b"keep-assets-file\n")
        before = sentinel.read_bytes()
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False,
                              expected_exit=3)
        require("EXISTS" in proc.stderr,
                "regular-file NEW.assets was not reported EXISTS")
        h.assert_absent(new, "NEW")
        require(sentinel.read_bytes() == before,
                "existing regular-file NEW.assets was changed")
    h.case("AC2.new-assets-regular-file-exists", "0036-R7-AC2",
           new_assets_regular_file_exists)

    def new_assets_dangling_symlink_exists():
        new = out / "ac2-existing-assets-dangling.img"
        sentinel = assets_path(new)
        os.symlink("missing-assets-target", sentinel)
        proc = h.migrate_fail(f["v1"], new, 1023, no_residue=False,
                              expected_exit=3)
        require("EXISTS" in proc.stderr,
                "dangling-symlink NEW.assets was not reported EXISTS")
        h.assert_absent(new, "NEW")
        require(os.path.islink(sentinel), "existing dangling NEW.assets was replaced")
        require(os.readlink(sentinel) == "missing-assets-target",
                "existing dangling NEW.assets target changed")
    h.case("AC2.new-assets-dangling-symlink-exists", "0036-R7-AC2",
           new_assets_dangling_symlink_exists)

    def lock_busy():
        old = f["v1"]
        new = out / "ac2-lock-busy.img"
        lock = Path(os.path.realpath(str(old)) + ".lock")
        fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            proc = h.migrate_fail(old, new, 1023, expected_exit=4)
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)
        require("BUSY" in proc.stderr, "held exclusive lock was not reported BUSY")
    h.case("AC2.old-lock-exclusive-busy", "0036-R7-AC2", lock_busy)

    def blocks_too_small():
        proc = h.migrate_fail(f["v1"], out / "ac2-blocks-small.img", 1023, 20, expected_exit=5)
        require("NOSPACE" in proc.stderr, "insufficient --blocks was not NOSPACE")
    h.case("AC2.blocks-too-small", "0036-R7-AC2", blocks_too_small)

    def corrupt_source():
        old = f["corrupt"]
        before = sha256_file(old)
        proc = h.run(h.cmd(old, out / "ac2-corrupt-source.img", 1023))
        require(proc.returncode == 6,
                f"corrupt source exit {proc.returncode} != 6")
        require(proc.stdout == "", "corrupt source printed stdout")
        require("CORRUPT" in proc.stderr, "corrupt source did not report CORRUPT")
        h.assert_unchanged(old, before, "corrupt OLD")
        h.assert_absent(out / "ac2-corrupt-source.img", "NEW")
        h.assert_absent(assets_path(out / "ac2-corrupt-source.img"), "NEW.assets")
    h.case("AC2.corrupt-source", "0036-R7-AC2", corrupt_source)

    def old_readonly():
        old = f["v1"]
        h.migrate_ok(old, out / "ac2-readonly.img", 1023)
        os.chmod(old, 0o444)
        try:
            h.migrate_ok(old, out / "ac2-readonly-success.img", 1023)
        finally:
            os.chmod(old, 0o644)
    h.case("AC2.old-readonly-chmod0444", "0036-R7-AC2", old_readonly)

    def old_open_readonly_audit():
        old = f["v1"]
        new = out / "ac2-audit.img"
        env = dict(os.environ)
        env["MIG_FAULT"] = "audit"
        before = sha256_file(old)
        proc = h.run([str(h.fault), "--from", str(old), "--to", str(new),
                      "--files", "1023"], env=env)
        require(proc.returncode == 0, f"audit migration failed: {proc.stderr.strip()[:400]}")
        target_real = os.path.realpath(str(old))
        saw_old = False
        for line in proc.stderr.splitlines():
            if not line.startswith("EVENT open path="):
                continue
            fields = line.split(" flags=")
            path = fields[0][len("EVENT open path="):]
            flags = int(fields[1])
            if os.path.realpath(path) != target_real:
                continue
            saw_old = True
            forbidden = os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC
            require((flags & forbidden) == 0,
                    f"OLD opened with write flags {flags:#x}")
        require(saw_old, "audit wrapper did not observe the OLD open")
        require("EVENT old_fd=" in proc.stderr,
                "audit wrapper did not track the OLD file descriptor")
        require("EVENT old_write" not in proc.stderr,
                "audit wrapper observed a write/pwrite on OLD")
        h.assert_unchanged(old, before, "OLD")
    h.case("AC2.old-open-flags-readonly", "0036-R7-AC2", old_open_readonly_audit)

    def alias_to_old():
        old = f["alias_old"]
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        proc = h.run(h.cmd(old, old, 1023))
        require(proc.returncode == 2, f"--to OLD exit {proc.returncode} != 2")
        require(proc.stdout == "", "--to OLD printed stdout")
        h.assert_unchanged(old, before, "alias OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                "alias OLD.assets changed")
        h.assert_absent(assets_path(old), "alias OLD.assets")
    h.case("AC2.alias-to-old-rejected", "0036-R7-AC2", alias_to_old)

    def alias_inside_assets():
        old = f["alias_old"]
        assets = assets_path(old)
        assets.mkdir(exist_ok=True)
        before = asset_manifest(assets)
        before_image = sha256_file(old)
        new = assets / "new.img"
        proc = h.run(h.cmd(old, new, 1023))
        require(proc.returncode == 2,
                f"--to OLD.assets/... exit {proc.returncode} != 2")
        require(proc.stdout == "", "--to OLD.assets/... printed stdout")
        h.assert_unchanged(old, before_image, "alias OLD")
        h.assert_absent(new, "NEW inside OLD.assets")
        h.assert_absent(assets_path(new), "NEW.assets inside OLD.assets")
        require(asset_manifest(assets) == before, "OLD.assets changed")
    h.case("AC2.alias-inside-old-assets-rejected", "0036-R7-AC2", alias_inside_assets)

    def assets_source_regular():
        old = f["assets_file"]
        sentinel = Path(str(old) + ".assets")
        before = sentinel.read_bytes()
        before_image = sha256_file(old)
        proc = h.run(h.cmd(old, out / "ac2-assets-file.img", 1023))
        require(proc.returncode == 7,
                f"regular-file OLD.assets exit {proc.returncode} != 7")
        require(proc.stdout == "", "regular-file OLD.assets printed stdout")
        require(sentinel.read_bytes() == before, "OLD.assets regular file changed")
        h.assert_unchanged(old, before_image, "OLD")
        h.assert_absent(out / "ac2-assets-file.img", "NEW")
        h.assert_absent(assets_path(out / "ac2-assets-file.img"), "NEW.assets")
    h.case("AC2.assets-source-regular-file", "0036-R7-AC2", assets_source_regular)

    def assets_source_symlink():
        old = f["assets_link"]
        link = Path(str(old) + ".assets")
        target = os.readlink(link)
        before_image = sha256_file(old)
        proc = h.run(h.cmd(old, out / "ac2-assets-link.img", 1023))
        require(proc.returncode == 7,
                f"symlink OLD.assets exit {proc.returncode} != 7")
        require(proc.stdout == "", "symlink OLD.assets printed stdout")
        require(os.path.islink(link) and os.readlink(link) == target,
                "OLD.assets symlink changed")
        h.assert_unchanged(old, before_image, "OLD")
        h.assert_absent(out / "ac2-assets-link.img", "NEW")
        h.assert_absent(assets_path(out / "ac2-assets-link.img"), "NEW.assets")
    h.case("AC2.assets-source-symlink", "0036-R7-AC2", assets_source_symlink)

    def assets_child_symlink():
        old = f["assets_child_link"]
        before = asset_manifest(assets_path(old))
        before_image = sha256_file(old)
        proc = h.run(h.cmd(old, out / "ac2-assets-child-link.img", 1023))
        require(proc.returncode == 7,
                f"asset child symlink exit {proc.returncode} != 7")
        require(proc.stdout == "", "asset child symlink printed stdout")
        require("unsupported" in proc.stderr.lower(), "symlink failure was not explicit")
        h.assert_unchanged(old, before_image, "OLD")
        h.assert_absent(out / "ac2-assets-child-link.img", "NEW")
        h.assert_absent(assets_path(out / "ac2-assets-child-link.img"), "NEW.assets")
        require(asset_manifest(assets_path(old)) == before, "OLD.assets changed")
    h.case("AC2.assets-child-symlink-cleanup", "0036-R7-AC2", assets_child_symlink)

    def assets_child_fifo():
        old = f["assets_child_fifo"]
        before = asset_manifest(assets_path(old))
        before_image = sha256_file(old)
        proc = h.run(h.cmd(old, out / "ac2-assets-child-fifo.img", 1023))
        require(proc.returncode == 7,
                f"asset child FIFO exit {proc.returncode} != 7")
        require(proc.stdout == "", "asset child FIFO printed stdout")
        require("unsupported" in proc.stderr.lower(), "FIFO failure was not explicit")
        h.assert_unchanged(old, before_image, "OLD")
        h.assert_absent(out / "ac2-assets-child-fifo.img", "NEW")
        h.assert_absent(assets_path(out / "ac2-assets-child-fifo.img"), "NEW.assets")
        require(asset_manifest(assets_path(old)) == before, "OLD.assets changed")
    h.case("AC2.assets-child-fifo-cleanup", "0036-R7-AC2", assets_child_fifo)

    def assets_empty_dir_copied():
        image = h.migrate_ok(f["v1"], out / "ac2-assets-empty.img", 1023)
        require((assets_path(out / "ac2-assets-empty.img") / "empty").is_dir(),
                "empty asset directory was not copied")
        require(image.live, "migration unexpectedly empty")
    h.case("AC2.assets-empty-and-deep-copy", "0036-R7-AC2", assets_empty_dir_copied)


def add_cli_boundary_cases(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_cli"
    out.mkdir(exist_ok=True)
    old = f["v1_empty"]
    cases = [
        ("files-0", ["--from", str(old), "--to", str(out / "cli-files-0.img"),
                     "--files", "0"]),
        ("files-64", ["--from", str(old), "--to", str(out / "cli-files-64.img"),
                      "--files", "64"]),
        ("files-100", ["--from", str(old), "--to", str(out / "cli-files-100.img"),
                       "--files", "100"]),
        ("files-4096", ["--from", str(old), "--to", str(out / "cli-files-4096.img"),
                        "--files", "4096"]),
        ("files-8191", ["--from", str(old), "--to", str(out / "cli-files-8191.img"),
                        "--files", "8191"]),
        ("files-nondecimal", ["--from", str(old),
                              "--to", str(out / "cli-files-nondecimal.img"),
                              "--files", "0x3ff"]),
        ("blocks-0", ["--from", str(old), "--to", str(out / "cli-blocks-0.img"),
                      "--files", "1023", "--blocks", "0"]),
        ("blocks-negative", ["--from", str(old),
                             "--to", str(out / "cli-blocks-negative.img"),
                             "--files", "1023", "--blocks", "-1"]),
        ("blocks-nondecimal", ["--from", str(old),
                               "--to", str(out / "cli-blocks-nondecimal.img"),
                               "--files", "1023", "--blocks", "4k"]),
        ("blocks-overflow", ["--from", str(old),
                             "--to", str(out / "cli-blocks-overflow.img"),
                             "--files", "1023", "--blocks", "4294967296"]),
        ("duplicate-files", ["--from", str(old),
                             "--to", str(out / "cli-duplicate-files.img"),
                             "--files", "1023", "--files", "1023"]),
        ("duplicate-to", ["--from", str(old),
                          "--to", str(out / "cli-duplicate-to.img"),
                          "--to", str(out / "cli-duplicate-to.img"),
                          "--files", "1023"]),
        ("unknown-option", ["--from", str(old),
                            "--to", str(out / "cli-unknown.img"),
                            "--files", "1023", "--bogus", "x"]),
        ("missing-files-value", ["--from", str(old),
                                 "--to", str(out / "cli-missing-files-value.img"),
                                 "--files"]),
        ("missing-files", ["--from", str(old),
                           "--to", str(out / "cli-missing-files.img")]),
        ("missing-from", ["--to", str(out / "cli-missing-from.img"),
                          "--files", "1023"]),
        ("missing-to", ["--from", str(old), "--files", "1023"]),
        ("empty-from", ["--from", "", "--to", str(out / "cli-empty-from.img"),
                        "--files", "1023"]),
        ("empty-to", ["--from", str(old), "--to", "", "--files", "1023"]),
    ]
    for name, arguments in cases:
        def one(arguments=arguments, name=name):
            before = sha256_file(old)
            before_assets = asset_manifest(assets_path(old))
            proc = h.run([str(h.migrate)] + arguments)
            require(proc.returncode == 2,
                    f"CLI boundary {name}: exit {proc.returncode} != 2; "
                    f"stderr={proc.stderr.strip()[:300]}")
            require(proc.stdout == "",
                    f"CLI boundary {name}: wrote stdout {proc.stdout!r}")
            require(bool(proc.stderr.strip()),
                    f"CLI boundary {name}: no diagnostic")
            h.assert_unchanged(old, before, "OLD")
            require(asset_manifest(assets_path(old)) == before_assets,
                    f"CLI boundary {name}: OLD.assets changed")
            for token in arguments:
                if token.startswith(str(out)) and token.endswith(".img"):
                    h.assert_absent(Path(token), f"NEW for {name}")
                    h.assert_absent(assets_path(Path(token)),
                                    f"NEW.assets for {name}")
        h.case(f"CLI.{name}", "0036-R7-AC2", one)


def add_fault_cases(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_out"
    out.mkdir(exist_ok=True)
    old = f["fault_old"]
    cases = [
        ("dir_write", "FAULT.dir-write"),
        ("data_write", "FAULT.data-write"),
        ("asset_file_write", "FAULT.asset-file-write"),
        ("asset_dir_fsync", "FAULT.asset-dir-fsync"),
        ("image_fsync", "FAULT.image-fsync"),
        ("verify_open", "FAULT.verify-reopen"),
        ("verify_read", "FAULT.verify-read-compare"),
        ("target_close", "FAULT.target-close"),
        ("verify_close", "FAULT.verify-close"),
        ("parent_close", "FAULT.parent-close"),
    ]
    for kind, name in cases:
        def fault_case(kind=kind):
            new = out / f"fault-{kind}.img"
            before = sha256_file(old)
            before_assets = asset_manifest(assets_path(old))
            env = dict(os.environ)
            env["MIG_FAULT"] = kind
            proc = h.run([str(h.fault), "--from", str(old), "--to", str(new),
                          "--files", "1023"], env=env)
            stderr = proc.stderr
            require(proc.returncode == 8,
                    f"fault {kind} exit {proc.returncode} != 8")
            require(proc.stdout == "", f"fault {kind} printed stdout")
            require("EVENT new_created" in stderr,
                    f"fault {kind} did not prove target creation")
            marker = f"FAULT kind={kind}"
            require(marker in stderr, f"missing injection marker {marker}")
            require("after_new=1" in stderr.split(marker, 1)[1].splitlines()[0],
                    f"fault {kind} was injected before NEW creation")
            require("MIGFAULT result=0" not in stderr,
                    f"fault {kind} was swallowed as success")
            h.assert_absent(new, "NEW")
            h.assert_absent(assets_path(new), "NEW.assets")
            h.assert_unchanged(old, before, "OLD")
            require(asset_manifest(assets_path(old)) == before_assets,
                    f"fault {kind} changed OLD.assets")
        h.case(name, "0036-R7-fault", fault_case)

    def cleanup_failure_reported():
        new = out / "fault-cleanup-failure.img"
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        env = dict(os.environ)
        env["MIG_FAULT"] = "data_write"
        env["MIG_FAIL_CLEANUP"] = "1"
        proc = h.run([str(h.fault), "--from", str(old), "--to", str(new),
                      "--files", "1023"], env=env)
        require(proc.returncode == 8,
                f"cleanup failure exit {proc.returncode} != 8")
        require(proc.stdout == "", "cleanup failure printed stdout")
        require("cleanup_unlink" in proc.stderr, "cleanup unlink fault did not fire")
        require(os.path.lexists(new), "cleanup failure did not leave a residual")
        require("residual" in proc.stderr.lower(),
                "residual path was not named in the diagnostic")
        require(str(new) in proc.stderr or new.name in proc.stderr,
                "cleanup diagnostic does not contain the residual path")
        h.assert_unchanged(old, before, "OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                "cleanup failure changed OLD.assets")
    h.case("FAULT.cleanup-failure-explicit-residual", "0036-R7-fault",
           cleanup_failure_reported)

    def cleanup_assets_failure_reported():
        new = out / "fault-cleanup-assets-failure.img"
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        env = dict(os.environ)
        env["MIG_FAULT"] = "asset_file_write"
        env["MIG_FAIL_CLEANUP_ASSETS"] = "1"
        proc = h.run([str(h.fault), "--from", str(old), "--to", str(new),
                      "--files", "1023"], env=env)
        require(proc.returncode == 8,
                f"asset cleanup failure exit {proc.returncode} != 8")
        require(proc.stdout == "", "asset cleanup failure printed stdout")
        require("cleanup_assets_unlink" in proc.stderr,
                "cleanup asset unlink fault did not fire")
        h.assert_absent(new, "NEW")
        require(os.path.lexists(assets_path(new)),
                "asset cleanup failure did not leave the NEW.assets residual")
        require("residual" in proc.stderr.lower(),
                "asset residual path was not named in the diagnostic")
        require(str(assets_path(new)) in proc.stderr or
                assets_path(new).name in proc.stderr,
                "asset cleanup diagnostic does not contain the residual path")
        h.assert_unchanged(old, before, "OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                "asset cleanup failure changed OLD.assets")
    h.case("FAULT.cleanup-assets-failure-explicit-residual", "0036-R7-fault",
           cleanup_assets_failure_reported)


def add_robustness_cases(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_robust"
    out.mkdir(exist_ok=True)
    old = f["fault_old"]

    def successful_write_retains_eintr():
        new = out / "robust-eintr.img"
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        old_image = decode_image(old)
        env = dict(os.environ)
        env["MIG_FAULT"] = "errno_eintr"
        proc = h.run([str(h.fault), "--from", str(old), "--to", str(new),
                      "--files", "1023"], env=env)
        require(proc.returncode == 0,
                f"successful write with EINTR errno failed: {proc.returncode}; "
                f"{proc.stderr.strip()[:400]}")
        require(proc.stdout == "", "errno-retention success printed stdout")
        new_image = decode_image(new)
        h.compare_images(old_image, new_image, 1023, 271)
        require(asset_manifest(assets_path(new)) == before_assets,
                "errno-retention success copied assets incorrectly")
        h.assert_unchanged(old, before, "OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                "errno-retention success changed OLD.assets")
    h.case("ROBUST.successful-write-retains-eintr", "0036-R7-robustness",
           successful_write_retains_eintr)

    def readdir_failure(kind, name, residual):
        new = out / f"readdir-{kind}.img"
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        env = dict(os.environ)
        env["MIG_FAULT"] = kind
        if residual:
            env["MIG_FAULT"] = "asset_file_write"
            env["MIG_SECONDARY"] = "readdir_remove"
        proc = h.run([str(h.fault), "--from", str(old), "--to", str(new),
                      "--files", "1023"], env=env)
        require(proc.returncode == 8,
                f"{name}: exit {proc.returncode} != 8; {proc.stderr.strip()[:300]}")
        require(proc.stdout == "", f"{name}: printed stdout")
        if residual:
            require("FAULT kind=asset_file_write after_new=1" in proc.stderr,
                    f"{name}: primary fault marker missing")
            require("FAULT kind=readdir_remove after_new=1" in proc.stderr,
                    f"{name}: remove readdir marker missing")
            h.assert_absent(new, "NEW")
            require(os.path.lexists(assets_path(new)),
                    f"{name}: NEW.assets residual missing")
            require("residual" in proc.stderr.lower(),
                    f"{name}: residual path not named")
            require(str(assets_path(new)) in proc.stderr or
                    assets_path(new).name in proc.stderr,
                    f"{name}: asset residual path not in diagnostic")
        else:
            marker = f"FAULT kind={kind} after_new=1"
            require(marker in proc.stderr, f"{name}: marker {marker} missing")
            h.assert_absent(new, "NEW")
            h.assert_absent(assets_path(new), "NEW.assets")
        h.assert_unchanged(old, before, "OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                f"{name}: OLD.assets changed")
    h.case("READDIR.copy-main-eio", "0036-R7-robustness",
           lambda: readdir_failure("readdir_copy", "copy-main-eio", False))
    h.case("READDIR.verify-target-count-eio", "0036-R7-robustness",
           lambda: readdir_failure("readdir_verify_target",
                                   "verify-target-count-eio", False))
    h.case("READDIR.remove-eio-residual", "0036-R7-robustness",
           lambda: readdir_failure("readdir_remove", "remove-eio", True))


def add_term_cases(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_out"
    out.mkdir(exist_ok=True)
    queries = ["shared", "zebrafish", "标签"]

    def rebuild_equivalence():
        old = f["v1"]
        new = out / "term-new.img"
        h.migrate_ok(old, new, 1023)
        old_output = run_terms_probe(h, old, queries)
        new_output = run_terms_probe(h, new, queries)
        # Capacity/D/N legitimately differ between a v1 source and a v2
        # target; the 2026-10-21 clarification compares index state and query
        # results, not the image geometry line.
        old_semantics = "\n".join(line for line in old_output.splitlines()
                                  if not line.startswith("IMAGE "))
        new_semantics = "\n".join(line for line in new_output.splitlines()
                                   if not line.startswith("IMAGE "))
        require(old_semantics == new_semantics,
                "term index/search output differs between OLD and NEW")
        for label, output in (("OLD", old_output), ("NEW", new_output)):
            require("REBUILD result=0" in output,
                    f"{label} rebuild was not OK")
            require(f"INDEX state={READY}" in output,
                    f"{label} term index is not READY")
            require("MATCH name=" in output,
                    f"{label} produced no match for the fixture queries")
            require("snippet=" in output, f"{label} did not print raw snippet bytes")
        require("SUMMARY result=0 total=0 " not in old_output,
                "all term queries were empty; equivalence would be vacuous")
    h.case("TERM.fresh-process-rebuild-equivalent", "0036-R7-AC1",
           rebuild_equivalence)


def add_oracle_self_checks(h: Harness, f: dict[str, Path]) -> None:
    out = h.work / "yanfs_migrate_out"
    out.mkdir(exist_ok=True)

    def corrupt_candidate_data():
        old = f["v1"]
        new = out / "oracle-good.img"
        h.migrate_ok(old, new, 1023)
        image = decode_image(new)
        victim = next(entry for entry in image.live if entry.size >= 2)
        bad = out / "oracle-data-corrupt.img"
        raw = bytearray(image.raw)
        raw[victim.start * BLOCK + victim.size - 1] ^= 0xFF
        bad.write_bytes(raw)
        corrupted = decode_image(bad)
        require(source_sha256(corrupted) != source_sha256(decode_image(old)),
                "one-byte data corruption was not detected by the identity/bytes oracle")
    h.case("ORACLE.corrupt-candidate-data-detected", "0036-R7-detection",
           corrupt_candidate_data)

    def corrupt_candidate_crc():
        new = out / "oracle-good.img"
        if not new.exists():
            h.migrate_ok(f["v1"], new, 1023)
        bad = out / "oracle-crc-corrupt.img"
        raw = bytearray(new.read_bytes())
        raw[CRC_OFFSET] ^= 0xFF
        bad.write_bytes(raw)
        try:
            decode_image(bad)
        except OracleError:
            return
        raise CaseFailure("flipped CRC byte was accepted by the independent decoder")
    h.case("ORACLE.corrupt-candidate-crc-detected", "0036-R7-detection",
           corrupt_candidate_crc)

    def corrupt_candidate_name():
        new = out / "oracle-good.img"
        if not new.exists():
            h.migrate_ok(f["v1"], new, 1023)
        bad = out / "oracle-name-corrupt.img"
        raw = bytearray(new.read_bytes())
        # slot 0 name terminator becomes a non-zero padding byte.
        raw[HEADER_BYTES + len("alpha.txt") + 1] = 0x41
        bad.write_bytes(raw)
        try:
            decode_image(bad)
        except OracleError:
            return
        raise CaseFailure("non-zero name padding was accepted by the independent decoder")
    h.case("ORACLE.corrupt-candidate-name-detected", "0036-R7-detection",
           corrupt_candidate_name)


def add_detection_case(h: Harness, args: argparse.Namespace, f: dict[str, Path]) -> None:
    if not args.corrupt and args.source_root:
        try:
            args.corrupt = str(build_bad_candidate(
                args.cc, Path(args.source_root).resolve(),
                h.work / "detection-builds", "image"))
        except CompilerMissing as error:
            h.record("DETECTION.controlled-corrupt-candidate",
                     "0036-R7-detection", "SKIP", str(error))
            h.exit_code = 77
            return
        except CaseFailure as error:
            h.record("DETECTION.controlled-corrupt-candidate",
                     "0036-R7-detection", "FAIL", str(error))
            return
    if not args.corrupt:
        h.record("DETECTION.controlled-corrupt-candidate", "0036-R7-detection",
                 "FAIL" if args.require_detection else "SKIP",
                 "--corrupt not supplied and no --source-root to build it"
                 if args.require_detection else
                 "--corrupt not supplied; candidate-level detection not run")
        return
    binary = Path(args.corrupt).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        h.record("DETECTION.controlled-corrupt-candidate", "0036-R7-detection",
                 "SKIP", f"{binary} is not executable")
        return
    out = h.work / "yanfs_migrate_detection"
    out.mkdir(exist_ok=True)

    def corrupt_candidate_caught():
        old = f["v1"]
        new = out / "controlled-corrupt.img"
        before = sha256_file(old)
        proc = h.run([str(binary), "--from", str(old), "--to", str(new),
                      "--files", "1023"])
        require(proc.returncode == 0,
                "controlled candidate was expected to report success, "
                f"got {proc.returncode}: {proc.stderr.strip()[:300]}")
        require(proc.stdout.startswith("yan_migrate: migrated "),
                "controlled candidate did not print its normal success line")
        produced = decode_image(new)
        original = decode_image(old)
        same = (source_sha256(produced) == source_sha256(original) and
                [e.data for e in produced.live] == [e.data for e in original.live])
        require(not same,
                "controlled one-byte corruption was NOT detected by the independent oracle")
        h.assert_unchanged(old, before, "OLD")
    h.case("DETECTION.controlled-corrupt-candidate", "0036-R7-detection",
           corrupt_candidate_caught)


def add_asset_detection_case(h: Harness, args: argparse.Namespace,
                             f: dict[str, Path]) -> None:
    if not args.corrupt_assets and args.source_root:
        try:
            args.corrupt_assets = str(build_bad_candidate(
                args.cc, Path(args.source_root).resolve(),
                h.work / "detection-builds", "assets"))
        except CompilerMissing as error:
            h.record("DETECTION.controlled-corrupt-assets",
                     "0036-R7-detection", "SKIP", str(error))
            h.exit_code = 77
            return
        except CaseFailure as error:
            h.record("DETECTION.controlled-corrupt-assets",
                     "0036-R7-detection", "FAIL", str(error))
            return
    if not args.corrupt_assets:
        h.record("DETECTION.controlled-corrupt-assets", "0036-R7-detection",
                 "FAIL" if args.require_detection else "SKIP",
                 "--corrupt-assets not supplied and no --source-root to build it"
                 if args.require_detection else
                 "--corrupt-assets not supplied; asset-level detection not run")
        return
    binary = Path(args.corrupt_assets).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        h.record("DETECTION.controlled-corrupt-assets", "0036-R7-detection",
                 "SKIP", f"{binary} is not executable")
        return
    out = h.work / "yanfs_migrate_detection"
    out.mkdir(exist_ok=True)

    def corrupt_assets_caught():
        old = f["v1"]
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        new = out / "controlled-corrupt-assets.img"
        proc = h.run([str(binary), "--from", str(old), "--to", str(new),
                      "--files", "1023"])
        require(proc.returncode == 0,
                "asset-corrupt candidate was expected to report success, "
                f"got {proc.returncode}: {proc.stderr.strip()[:300]}")
        require(proc.stdout.startswith("yan_migrate: migrated "),
                "asset-corrupt candidate did not print its normal success line")
        produced = decode_image(new)
        original = decode_image(old)
        require(source_sha256(produced) == source_sha256(original) and
                [e.data for e in produced.live] == [e.data for e in original.live],
                "asset-corrupt candidate also changed the image; boundary is not asset-only")
        require(asset_manifest(assets_path(new)) != before_assets,
                "asset corruption was NOT detected by the independent asset manifest")
        h.assert_unchanged(old, before, "OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                "OLD.assets changed during asset-corrupt candidate run")
    h.case("DETECTION.controlled-corrupt-assets", "0036-R7-detection",
           corrupt_assets_caught)


def make_long_target(h: Harness, label: str, length: int = 731) -> Path:
    target = h.work / "cli-fault-long" / label
    target.mkdir(parents=True)
    index = 0
    while True:
        remaining = length - len(str(target)) - 1
        if remaining <= 200:
            break
        target = target / (("p" * 200) + f"_{index}")
        index += 1
        target.mkdir()
    remaining = length - len(str(target)) - 1
    if not 1 <= remaining <= 255:
        raise CaseFailure(
            f"cannot build a {length}-byte long path (remaining {remaining})")
    target = target / ("n" * remaining)
    if len(str(target)) != length:
        raise CaseFailure(f"long path length {len(str(target))} != {length}")
    return target


def add_cli_fault_cases(h: Harness, args: argparse.Namespace,
                        f: dict[str, Path]) -> None:
    binary: Path | None = None
    if args.cli_fault:
        binary = Path(args.cli_fault).resolve()
    elif args.source_root:
        try:
            binary = build_cli_fault(
                args.cc, Path(args.source_root).resolve(),
                h.work / "cli-fault-build" / "yan_migrate_cli_fault")
        except CompilerMissing as error:
            h.record("CLI-FAULT.actual-cli-build", "0036-R7-fault", "SKIP",
                     str(error))
            h.exit_code = 77
            return
        except CaseFailure as error:
            h.record("CLI-FAULT.actual-cli-build", "0036-R7-fault", "FAIL",
                     str(error))
            return
    if binary is None or not binary.is_file() or not os.access(binary, os.X_OK):
        h.record("CLI-FAULT.actual-cli-build", "0036-R7-fault",
                 "FAIL" if args.require_cli_fault else "SKIP",
                 "--cli-fault not supplied and no --source-root to build it")
        return
    out = h.work / "yanfs_migrate_cli_fault"
    out.mkdir(exist_ok=True)

    def cli_fault_case(mode: str, name: str, both_residual: bool):
        new = make_long_target(h, name)
        h.assert_absent(new, "NEW before run")
        h.assert_absent(assets_path(new), "NEW.assets before run")
        old = f["v1"]
        before = sha256_file(old)
        before_assets = asset_manifest(assets_path(old))
        env = dict(os.environ)
        env["YANOS_CLI_FAULT"] = mode
        env["YANOS_CLI_TARGET_BASE"] = new.name
        env["YANOS_CLI_ASSETS_BASE"] = new.name + ".assets"
        proc = h.run([str(binary), "--from", str(old), "--to", str(new),
                      "--files", "1023"], env=env)
        require(proc.returncode == 8,
                f"{name}: actual CLI exit {proc.returncode} != 8; "
                f"stderr={proc.stderr[-400:]}")
        require(proc.stdout == "", f"{name}: actual CLI failure printed stdout")
        require(str(new) in proc.stderr,
                f"{name}: full {len(str(new))}-byte NEW path missing from stderr")
        if both_residual:
            require(str(assets_path(new)) in proc.stderr,
                    f"{name}: full NEW.assets path missing from stderr")
            require(os.path.lexists(new), f"{name}: NEW residual missing")
            require(os.path.lexists(assets_path(new)),
                    f"{name}: NEW.assets residual missing")
        else:
            require(os.path.lexists(new), f"{name}: NEW residual missing")
            require(not os.path.lexists(assets_path(new)),
                    f"{name}: NEW.assets unexpectedly residual")
        h.assert_unchanged(old, before, "OLD")
        require(asset_manifest(assets_path(old)) == before_assets,
                f"{name}: OLD.assets changed")
    h.case("CLI-FAULT.target-unlink-long-residual", "0036-R7-fault",
           lambda: cli_fault_case("target-unlink", "target-unlink", False))
    h.case("CLI-FAULT.parent-reopen-long-two-residuals", "0036-R7-fault",
           lambda: cli_fault_case("parent-reopen", "parent-reopen", True))


def add_semantic_case(h: Harness, args: argparse.Namespace,
                      f: dict[str, Path]) -> None:
    required = args.require_semantic
    if not args.semantic or not args.runner:
        h.record("SEMANTIC.on-disk-index-ready", "0036-R7-AC1",
                 "FAIL" if required else "SKIP",
                 "no --semantic/--runner; AC1 on-disk semantic index NOT verified"
                 if required else
                 "no --semantic/--runner; AC1 on-disk semantic index NOT verified")
        return
    semantic = Path(args.semantic).resolve()
    runner = Path(args.runner).resolve()
    if not semantic.is_file() or not os.access(semantic, os.X_OK):
        h.record("SEMANTIC.on-disk-index-ready", "0036-R7-AC1",
                 "FAIL" if required else "SKIP",
                 f"semantic not executable: {semantic}")
        return
    if not runner.is_file() or not os.access(runner, os.X_OK):
        h.record("SEMANTIC.on-disk-index-ready", "0036-R7-AC1",
                 "FAIL" if required else "SKIP",
                 f"runner not executable: {runner}")
        return
    out = h.work / "yanfs_migrate_semantic"
    out.mkdir(exist_ok=True)
    model = Path(args.model).resolve() if args.model else out / "semantic-model"
    if args.model:
        if not model.is_file():
            h.record("SEMANTIC.on-disk-index-ready", "0036-R7-AC1",
                     "FAIL" if required else "SKIP",
                     f"model not available: {model}")
            return
    else:
        model.write_bytes(b"universal")
    index = Path(args.index).resolve() if args.index else out / "semantic-index"
    if index.exists():
        index.unlink()

    def semantic_ready_after_migration():
        base = [str(semantic)]
        common = ["--index", str(index), "--model", str(model),
                  "--runner", str(runner),
                  "--startup-timeout", "60", "--request-timeout", "60"]
        old = f["v1"]
        rebuilt = h.run(base + ["rebuild", "--disk-image", str(old)] + common,
                        timeout=600)
        require(rebuilt.returncode == 0,
                f"OLD semantic rebuild failed: {rebuilt.stderr.strip()[:400]}")
        require("OK rebuild" in rebuilt.stdout,
                f"OLD semantic rebuild stdout: {rebuilt.stdout.strip()[:200]}")
        index_before = sha256_file(index)
        new = out / "semantic-new.img"
        h.migrate_ok(old, new, 1023)
        status = h.run(base + ["status", "--disk-image", str(new)] + common,
                       timeout=600)
        require(status.returncode == 0, "NEW semantic status failed")
        require("STATUS READY" in status.stdout,
                f"NEW semantic index is not READY: {status.stdout.strip()}")
        old_search = h.run(base + ["search", "--disk-image", str(old),
                                   "--query", "shared"] + common, timeout=600)
        new_search = h.run(base + ["search", "--disk-image", str(new),
                                   "--query", "shared"] + common, timeout=600)
        require(old_search.returncode == 0 and new_search.returncode == 0,
                "semantic search failed")
        require(old_search.stdout == new_search.stdout,
                "semantic search output differs between OLD and NEW")
        require(sha256_file(index) == index_before,
                "semantic index bytes changed during NEW status/search")
        require("MATCH " in old_search.stdout,
                "semantic search produced no MATCH rows")
        match = re.search(r"OK search shown=(\d+)", old_search.stdout)
        require(match is not None and int(match.group(1)) > 0,
                "semantic search produced zero shown rows")
    h.case("SEMANTIC.ready-and-search-equal", "0036-R7-AC1",
           semantic_ready_after_migration)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="0036-R7 independent acceptance")
    parser.add_argument("--migrate", required=True)
    parser.add_argument("--terms-probe", required=True)
    parser.add_argument("--fault", required=True)
    parser.add_argument("--semantic")
    parser.add_argument("--model")
    parser.add_argument("--runner")
    parser.add_argument("--index")
    parser.add_argument("--work", default=None)
    parser.add_argument("--corrupt",
                        help="optional controlled-corrupted candidate binary for "
                             "the detection-power case")
    parser.add_argument("--corrupt-assets",
                        help="optional controlled-corrupted-asset candidate binary")
    parser.add_argument("--source-root",
                        help="clean checkout root used to build controlled bad candidates")
    parser.add_argument("--require-detection", action="store_true",
                        help="fail when the controlled corruption candidates cannot run")
    parser.add_argument("--require-semantic", action="store_true",
                        help="fail instead of SKIP when the semantic index case cannot run")
    parser.add_argument("--cli-fault",
                        help="optional prebuilt actual-CLI fault binary")
    parser.add_argument("--require-cli-fault", action="store_true",
                        help="fail when the actual-CLI fault binary cannot be built")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--trace", action="store_true")
    return parser.parse_args(argv)


def validate_explicit_inputs(args: argparse.Namespace) -> bool:
    """Check every caller-supplied input before touching the work directory."""
    required = (("migrate", args.migrate, True),
                ("terms probe", args.terms_probe, True),
                ("fault binary", args.fault, True),
                ("cli-fault", args.cli_fault, True),
                ("semantic", args.semantic, True),
                ("corrupt", args.corrupt, True),
                ("corrupt-assets", args.corrupt_assets, True),
                ("runner", args.runner, True),
                ("model", args.model, False))
    for label, path, executable in required:
        if path is None:
            if label in ("migrate", "terms probe", "fault binary"):
                print(f"FATAL required artifact missing: {label}", file=sys.stderr)
                return False
            continue
        candidate = Path(path)
        readable = candidate.is_file()
        if executable:
            usable = readable and os.access(candidate, os.X_OK)
        else:
            usable = readable
        if not usable:
            print(f"FATAL explicit input missing or unusable: {label}={path}",
                  file=sys.stderr)
            return False
    if args.source_root is not None and not Path(args.source_root).is_dir():
        print(f"FATAL explicit input is not a directory: source-root={args.source_root}",
              file=sys.stderr)
        return False
    return True


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not validate_explicit_inputs(args):
        return 1
    root = Path(__file__).resolve().parent.parent
    default_work = root / "build" / "20261021-r7" / "b" / "run"
    work = (Path(args.work) if args.work else default_work).resolve()
    if work.exists() and not work.is_dir():
        print(f"FATAL --work is not a directory: {work}", file=sys.stderr)
        return 1
    work.mkdir(parents=True, exist_ok=True)
    run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=str(work)))
    print(f"WORK {run_dir}")

    harness = Harness(argparse.Namespace(**{**vars(args), "work": str(run_dir)}))
    try:
        fixtures = prepare_fixtures(run_dir)
    except Exception as error:  # noqa: BLE001
        print(f"FATAL fixture construction failed: {error}", file=sys.stderr)
        if args.trace:
            traceback.print_exc()
        print(f"RUN_DIR_KEPT {run_dir}", file=sys.stderr)
        return 1

    add_ac1_cases(harness, fixtures)
    add_ac2_cases(harness, fixtures)
    add_cli_boundary_cases(harness, fixtures)
    add_fault_cases(harness, fixtures)
    add_robustness_cases(harness, fixtures)
    add_cli_fault_cases(harness, args, fixtures)
    add_term_cases(harness, fixtures)
    add_oracle_self_checks(harness, fixtures)
    add_detection_case(harness, args, fixtures)
    add_asset_detection_case(harness, args, fixtures)
    add_semantic_case(harness, args, fixtures)

    print("\n=== 0036-R7 independent matrix ===")
    for name, ac, status, detail in harness.results:
        print(f"{status:4} {name:52} {ac:18} {detail}")
    print(f"\nPASS={harness.passes} FAIL={harness.failures} SKIP={harness.skips}")
    for temporary in fixtures.get("_temp_dirs", []):
        shutil.rmtree(temporary, ignore_errors=True)
    print(f"WORK={run_dir}")
    if harness.failures:
        return 1
    if harness.exit_code:
        return harness.exit_code
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
