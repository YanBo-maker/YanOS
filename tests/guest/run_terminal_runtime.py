#!/usr/bin/env python3
"""Real UART/PLIC/task-window and production-fault acceptance for 0022.

This builds and runs, from source in the repository:

  * the Host driver tests/terminal_runtime_driver.c, linked against the real
    machine library (src/ram.c, src/bus.c, src/machine.c, src/cpu.c, src/csr.c,
    src/interrupt.c, src/image.c, src/uart.c, src/transport.c) plus
    tools/host_block.c and tools/host_file.c, with a strict C17 host compiler
    and -Werror;
  * one Guest fixture per scenario from tests/guest/terminal_wait_check.c and
    the real platform sources (os/trap_entry.S, os/task_switch.S, os/task.c,
    os/console.c, os/line.c, os/terminal.c, os/memory.c) with os/guest.ld, for
    RV32IM freestanding at -O2;
  * the production ELF (--guest) through the same driver in production mode
    over a memory-backed canonical image this file builds itself.

Interface:
  --source ROOT   repository root (required)
  --work DIR      build and case directory (required)
  --gcc CROSS     RISC-V cross compiler (required)
  --cc CC         host C compiler (required)
  --guest ELF     the production terminal ELF (required)

Only the two external compilers can produce a skip (exit 77). A missing in-repo
source, a missing driver or a failure before any case runs is a hard failure
(exit 1). A build error, crash, timeout, sanitizer report or an unexpected
driver exit is a harness error (exit 2). A named scenario failure prints
":FAIL: <owner>: ..." and makes the script return 1.

Every scenario is recorded independently (command, stdout, stderr, return code,
driver status) under the work directory, and the script prints a final summary
count. A failing scenario does not hide the others; only a failing baseline
stops the fault runs that depend on it.
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

BLOCK = 4096
BLOCKS = 16
IMAGE_BYTES = BLOCK * BLOCKS
MAGIC = b"YANFS01\x00"

HOST_CC_FLAGS = ["-std=c17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
CROSS_FLAGS = [
    "-std=c17", "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany", "-static", "-nostdlib",
    "-nostartfiles", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
]
HOST_SOURCES = [
    "src/ram.c", "src/bus.c", "src/machine.c", "src/cpu.c", "src/csr.c",
    "src/interrupt.c", "src/image.c", "src/uart.c", "src/transport.c",
    "tools/host_block.c", "tools/host_file.c",
    "tests/terminal_runtime_driver.c",
]
GUEST_SOURCES = [
    "tests/guest/terminal_wait_check.c", "os/trap_entry.S", "os/task_switch.S",
    "os/task.c", "os/console.c", "os/line.c", "os/terminal.c", "os/memory.c",
]
GUEST_LD = "os/guest.ld"
# Every repository header the built sources include, so a missing one is a hard
# failure (1), not a silent build error.
HOST_HEADERS = [
    "tools/host_block.h", "tools/host_file.h",
    "include/yan/bus.h", "include/yan/cpu.h", "include/yan/image.h",
    "include/yan/interrupt.h", "include/yan/machine.h", "include/yan/ram.h",
    "include/yan/status.h", "include/yan/transport.h", "include/yan/uart.h",
]
GUEST_HEADERS = [
    "tests/guest/guest.h", "os/block.h", "os/console.h", "os/line.h",
    "os/platform.h", "os/shell.h", "os/task.h", "os/terminal.h", "os/yanfs.h",
    "os/yanfs_block.h",
]
SANITIZER_FLAGS = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-omit-frame-pointer"]

FIXTURE_SCENARIOS = [
    ("ready", 1),
    ("predicate", 2),
    ("blocked", 3),
    ("wake", 4),
    ("reject", 5),
]

TIMEOUT = 240
FIXTURE_STEPS = "400000"
PRODUCTION_STEPS = "200000000"
# The three application failure paths an output fault can take: the line echo
# (LINE_UNAVAILABLE), the shell's own emit (SHELL_FATAL) and the application's
# own message (OUTPUT).
TOHOST_LINE_UNAVAILABLE = 0x71000009
TOHOST_SHELL_FATAL = 0x7100000C
TOHOST_OUTPUT = 0x71000011
APP_FAIL_TOP = 0x71000000
APP_REASON_LAST = 17

# tests/guest/terminal_wait_check.c: TW_FAIL(reason) = 0x72000000 | reason.
FIXTURE_FAIL_TOP = 0x72000000
FIXTURE_FAIL_LAST = 17


class HarnessError(RuntimeError):
    """A condition that is not a clean pass/fail of a named assertion."""


class OwnerFailure(Exception):
    """A named scenario assertion failed."""


# ------------------------------------------------------------- image oracle

def reseal(block):
    crc = zlib.crc32(bytes(block[:60]) + b"\x00\x00\x00\x00"
                     + bytes(block[64:BLOCK])) & 0xFFFFFFFF
    struct.pack_into("<I", block, 60, crc)


def metadata_block(capacity):
    block = bytearray(BLOCK)
    block[0:8] = MAGIC
    struct.pack_into("<I", block, 8, 1)
    struct.pack_into("<I", block, 12, BLOCK)
    struct.pack_into("<I", block, 16, capacity)
    struct.pack_into("<I", block, 20, 63)
    reseal(block)
    return bytes(block)


def put_entry(block, slot, name, size, start, count):
    entry = bytearray(64)
    encoded = name.encode()
    entry[0:len(encoded)] = encoded
    struct.pack_into("<I", entry, 32, size)
    struct.pack_into("<I", entry, 36, start)
    struct.pack_into("<I", entry, 40, count)
    block[64 + slot * 64:64 + (slot + 1) * 64] = entry


def empty_image():
    image = bytearray(IMAGE_BYTES)
    image[0:BLOCK] = metadata_block(BLOCKS)
    return bytes(image)


def seeded_image(files):
    """files: [(name, payload, lba)]; count is ceil(size / block)."""
    image = bytearray(IMAGE_BYTES)
    block0 = bytearray(metadata_block(BLOCKS))
    for slot, (name, payload, lba) in enumerate(files):
        count = (len(payload) + BLOCK - 1) // BLOCK if payload else 0
        start = lba if payload else 0
        put_entry(block0, slot, name, len(payload), start, count)
        if payload:
            image[lba * BLOCK:lba * BLOCK + len(payload)] = payload
    reseal(block0)
    image[0:BLOCK] = block0
    return bytes(image)


# --------------------------------------------------------- display encoder

def _escape(seq):
    out = bytearray()
    for byte in seq:
        out.extend(b"\\x%02X" % byte)
    return out


def _render(text):
    """One decoded character: C0/DEL escaped, backslash doubled, a C1 control
    escaped as its own UTF-8 bytes, everything else as its own bytes."""
    cp = ord(text)
    if cp < 0x20 or cp == 0x7F:
        return _escape(bytes([cp]))
    if cp == 0x5C:
        return b"\\\\"
    if 0x80 <= cp <= 0x9F:
        return _escape(text.encode("utf-8"))
    return text.encode("utf-8")


def _candidate_length(byte):
    if byte < 0x80:
        return 1
    if 0xC0 <= byte <= 0xDF:
        return 2
    if 0xE0 <= byte <= 0xEF:
        return 3
    if 0xF0 <= byte <= 0xF7:
        return 4
    return 1


def display(data):
    """The 0022 cat rendering, built on Python's own strict UTF-8 decoder.

    At each position the candidate 1/2/3/4-byte sequence is handed to
    ``bytes.decode("utf-8", "strict")``; a valid candidate is rendered and
    skipped whole, anything else escapes exactly one byte and advances one. The
    oracle therefore never reimplements the reader's pending state machine:
    CPython itself rejects overlong, surrogate, out-of-range and truncated
    sequences, and each rejected byte is escaped byte by byte, which is the
    reader's own rule for invalid input."""
    out = bytearray()
    index = 0
    length = len(data)
    while index < length:
        candidate = _candidate_length(data[index])
        chunk = data[index:index + candidate]
        decoded = None
        if len(chunk) == candidate:
            try:
                text = chunk.decode("utf-8", "strict")
            except UnicodeDecodeError:
                text = None
            if text is not None and len(text) == 1:
                decoded = text
        if decoded is None:
            out.extend(_escape(data[index:index + 1]))
            index += 1
        else:
            out.extend(_render(decoded))
            index += candidate
    return bytes(out)


def display_self_check():
    """Explicit literal expectations for the known cat cases, so the oracle is
    pinned independently of any implementation state machine. True-EOF pending
    sequences of 1, 2 and 3 bytes are included."""
    cases = [
        (b"\xc2", b"\\xC2"),
        (b"\xe4\xb8", b"\\xE4\\xB8"),
        (b"\xf0\x9f\x92", b"\\xF0\\x9F\\x92"),
        (b"\xc2\xa0", b"\xc2\xa0"),
        (b"\xc2\x80", b"\\xC2\\x80"),
        (b"\xc2\x9f", b"\\xC2\\x9F"),
        (b"\xe4\xb8\xad", b"\xe4\xb8\xad"),
        (b"\xf0\x9f\x98\x80", b"\xf0\x9f\x98\x80"),
        (b"\\", b"\\\\"),
        (b"\x00", b"\\x00"),
        (b"\x7f", b"\\x7F"),
        (b"\x80", b"\\x80"),
        (b"\xc0\x80", b"\\xC0\\x80"),
        (b"\xed\xa0\x80", b"\\xED\\xA0\\x80"),
        (b"\xf4\x90\x80\x80", b"\\xF4\\x90\\x80\\x80"),
        (b"\xe4\xb8\xe4A", b"\\xE4\\xB8\\xE4A"),
    ]
    for data, expected in cases:
        got = display(data)
        if got != expected:
            raise AssertionError("display self-check: %r -> %r, expected %r"
                                 % (data, got, expected))


# ------------------------------------------------------------------ helpers

def run(argv, timeout=TIMEOUT):
    try:
        return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("timed out: %s" % " ".join(argv)) from error
    except OSError as error:
        raise HarnessError("cannot run %s: %s" % (argv[0], error)) from error


def tool_available(tool):
    if os.path.sep in tool:
        return os.path.isfile(tool) and os.access(tool, os.X_OK)
    from shutil import which
    return which(tool) is not None


STATUS_FIELDS = ["verdict", "tohost", "delivered", "out_length", "isr_count",
                 "predicate_entries", "checks", "reader_stage", "ticks",
                 "protocol_fired", "tx_fired"]
VERDICTS = {"pass", "guest", "drive", "harness", "timeout"}
VERDICT_FOR_EXIT = {0: "pass", 4: "timeout", 5: "harness", 6: "guest", 7: "drive"}


def parse_status(path, owner):
    """Read the driver's status file strictly: every field present, every value
    of the right shape. A read or shape failure is a harness error."""
    try:
        text = path.read_text()
    except OSError as error:
        raise HarnessError("%s: cannot read the driver status: %s"
                           % (owner, error)) from error
    values = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    missing = [field for field in STATUS_FIELDS if field not in values]
    if missing:
        raise HarnessError("%s: driver status missing %s"
                           % (owner, ", ".join(missing)))
    if values["verdict"] not in VERDICTS:
        raise HarnessError("%s: bad status verdict %r"
                           % (owner, values["verdict"]))
    try:
        tohost = int(values["tohost"], 16)
        numbers = {key: int(values[key]) for key in
                   ("delivered", "out_length", "isr_count", "predicate_entries",
                    "checks", "reader_stage", "ticks")}
    except ValueError as error:
        raise HarnessError("%s: non-numeric status field: %s"
                           % (owner, error)) from error
    for key in ("protocol_fired", "tx_fired"):
        if values[key] not in ("0", "1"):
            raise HarnessError("%s: %s is %r not 0/1" % (owner, key, values[key]))
    values.update(numbers)
    values["_tohost"] = tohost
    return values


def check_status_exit(owner, returncode, values):
    expected = VERDICT_FOR_EXIT.get(returncode)
    if expected is None:
        raise HarnessError("%s: driver exit %d has no status verdict"
                           % (owner, returncode))
    if values["verdict"] != expected:
        raise HarnessError("%s: driver exit %d but status verdict is %r"
                           % (owner, returncode, values["verdict"]))


def record(case_dir, label, argv, result):
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / (label + ".cmd")).write_text(" ".join(argv) + "\n")
    (case_dir / (label + ".stdout")).write_bytes(result.stdout)
    (case_dir / (label + ".stderr")).write_bytes(result.stderr)
    (case_dir / (label + ".rc")).write_text("%d\n" % result.returncode)


def last_line(result):
    lines = result.stderr.decode(errors="replace").strip().splitlines()
    return lines[-1] if lines else ""


def check_not_harness(result, owner):
    blob = result.stdout + result.stderr
    if result.returncode < 0:
        raise HarnessError("%s: died with signal %d" % (owner, -result.returncode))
    if result.returncode >= 128:
        raise HarnessError("%s: driver reported %d (128 + signal)"
                           % (owner, result.returncode))
    if (b"AddressSanitizer" in blob or b"LeakSanitizer" in blob
            or b"UndefinedBehaviorSanitizer" in blob or b"runtime error:" in blob):
        raise HarnessError("%s: sanitizer output" % owner)


# ------------------------------------------------------------------- build

def require_inputs(source, args):
    missing = []
    for rel in HOST_SOURCES + GUEST_SOURCES + HOST_HEADERS + GUEST_HEADERS + [GUEST_LD]:
        if not (source / rel).is_file():
            missing.append(str(source / rel))
    return missing


def build_host(source, work, cc, sanitizers=False):
    driver = work / "terminal_runtime_driver"
    flags = HOST_CC_FLAGS + (SANITIZER_FLAGS if sanitizers else [])
    argv = [cc] + flags + ["-I", str(source / "include"),
                           "-I", str(source / "tools")]
    argv += [str(source / rel) for rel in HOST_SOURCES]
    argv += ["-o", str(driver)]
    result = run(argv)
    if result.returncode != 0:
        raise HarnessError("host driver build failed:\n%s"
                           % result.stderr.decode(errors="replace"))
    return driver


def build_fixture(source, work, gcc, name, value):
    elf = work / ("fixture-%s.elf" % name)
    argv = [gcc] + CROSS_FLAGS + [
        "-I", str(source / "os"),
        "-I", str(source / "tests" / "guest"),
        "-DTW_SCENARIO=%d" % value,
        "-T", str(source / GUEST_LD),
        "-o", str(elf),
    ]
    argv += [str(source / rel) for rel in GUEST_SOURCES]
    result = run(argv)
    if result.returncode != 0:
        raise HarnessError("fixture '%s' build failed:\n%s"
                           % (name, result.stderr.decode(errors="replace")))
    return elf


# ------------------------------------------------------------------- cases

def run_fixture(driver, elf, work, scenario):
    owner = "fixture-" + scenario
    case_dir = work / owner
    case_dir.mkdir(parents=True, exist_ok=True)
    capture = case_dir / (scenario + ".out")
    status = case_dir / (scenario + ".status")
    argv = [str(driver), "--image", str(elf), "--mode", "fixture",
            "--scenario", scenario, "--capture", str(capture),
            "--status", str(status), "--max-steps", FIXTURE_STEPS]
    result = run(argv)
    record(case_dir, scenario, argv, result)
    check_not_harness(result, owner)
    if result.returncode == 2:
        raise HarnessError("%s: driver usage error 2" % owner)
    if result.returncode not in (0, 4, 5, 6, 7):
        raise HarnessError("%s: driver exit %d" % (owner, result.returncode))
    values = parse_status(status, owner)
    check_status_exit(owner, result.returncode, values)
    if result.returncode == 0:
        return
    if result.returncode in (4, 5):
        raise HarnessError("%s: driver exit %d (%s)"
                           % (owner, result.returncode, values["verdict"]))
    tohost = values["_tohost"]
    if result.returncode == 7:
        detail = last_line(result)
        if "[drive]" not in detail:
            raise HarnessError("%s: exit 7 without a [drive] assertion: %s"
                               % (owner, detail))
        raise OwnerFailure("%s: %s" % (owner, detail))
    if result.returncode == 6:
        if FIXTURE_FAIL_TOP + 1 <= tohost <= FIXTURE_FAIL_TOP + FIXTURE_FAIL_LAST:
            raise OwnerFailure("%s: Guest failure code 0x%08x" % (owner, tohost))
        raise HarnessError("%s: unexpected tohost 0x%08x (panic/trap)"
                           % (owner, tohost))
    raise HarnessError("%s: driver exit %d: %s"
                       % (owner, result.returncode, last_line(result)))


def run_production(driver, args, work, image, stdin_bytes, label, fault,
                   corrupt_response, tx_fault_at, baseline=None,
                   expect_tohost=None):
    case_dir = work / ("production-" + label)
    disk = case_dir / (label + ".img")
    stdin_path = case_dir / (label + ".stdin")
    capture = case_dir / (label + ".out")
    status = case_dir / (label + ".status")
    case_dir.mkdir(parents=True, exist_ok=True)
    disk.write_bytes(image)
    stdin_path.write_bytes(stdin_bytes)
    argv = [str(driver), "--image", str(args.guest), "--mode", "production",
            "--disk-image", str(disk), "--stdin", str(stdin_path),
            "--capture", str(capture), "--status", str(status),
            "--max-steps", PRODUCTION_STEPS, "--fault", fault,
            "--corrupt-response", str(corrupt_response),
            "--tx-fault-at", str(tx_fault_at)]
    if baseline is not None:
        argv += ["--baseline", str(baseline)]
    if expect_tohost is not None:
        argv += ["--expect-tohost", "0x%08x" % expect_tohost]
    result = run(argv)
    record(case_dir, label, argv, result)
    return result, status, disk, capture


def classify_production(result, status, owner, expected_codes):
    check_not_harness(result, owner)
    if result.returncode == 2:
        raise HarnessError("%s: driver usage error 2" % owner)
    values = parse_status(status, owner)
    check_status_exit(owner, result.returncode, values)
    if result.returncode == 7:
        detail = last_line(result)
        if "[drive]" not in detail:
            raise HarnessError("%s: exit 7 without a [drive] assertion: %s"
                               % (owner, detail))
        raise OwnerFailure("%s: %s" % (owner, detail))
    if result.returncode == 6:
        tohost = values["_tohost"]
        if tohost < APP_FAIL_TOP + 1 or tohost > APP_FAIL_TOP + APP_REASON_LAST:
            raise HarnessError("%s: tohost 0x%08x is not a known application"
                               " failure (panic/trap)" % (owner, tohost))
        if tohost not in expected_codes:
            raise OwnerFailure("%s: tohost 0x%08x is not one of %s"
                               % (owner, tohost,
                                  ", ".join("0x%08x" % code for code in expected_codes)))
        return values
    if result.returncode == 0:
        return values
    raise HarnessError("%s: driver exit %d (%s): %s"
                       % (owner, result.returncode, values["verdict"],
                          last_line(result)))


def case_production_faults(driver, args, work, results):
    baseline_dir = work / "production-baseline"
    baseline_dir.mkdir(parents=True, exist_ok=True)
    commands = b"create kept.txt x\nexit\n"
    result, status, disk, capture = run_production(
        driver, args, work, empty_image(), commands, "baseline", "none", 0, 0)
    values = classify_production(result, status, "production-baseline",
                                 {0})
    baseline_bytes = capture.read_bytes()
    if b"OK create\r\n" not in baseline_bytes or b"yanfs: exit\r\n" not in baseline_bytes:
        raise OwnerFailure("production-baseline: expected output is missing")
    if disk.read_bytes() != seeded_image([("kept.txt", b"x", 1)]):
        raise OwnerFailure("production-baseline: the final image oracle differs")
    results.append(("production-baseline", None))
    baseline_path = baseline_dir / "baseline.bin"
    baseline_path.write_bytes(baseline_bytes)

    prompt_at = baseline_bytes.find(b"yanfs> ")
    echo_at = baseline_bytes.find(b"create kept.txt")
    ok_at = baseline_bytes.find(b"OK create")
    ok_exit_at = baseline_bytes.find(b"OK exit\r\n")
    exit_at = baseline_bytes.find(b"yanfs: exit")
    for phase, position in (("prompt", prompt_at), ("echo", echo_at),
                            ("receipt", ok_at), ("exit-receipt", ok_exit_at),
                            ("exit", exit_at)):
        if position < 0:
            raise HarnessError("production-baseline: cannot locate the %s phase"
                               % phase)
    exit_line = b"yanfs: exit\r\n"
    # Each stage's expected tohost is the exact application path that byte
    # belongs to: the application's own banner/prompt/exit (OUTPUT), the line
    # echo (LINE_UNAVAILABLE), and the shell's success receipt (SHELL_FATAL).
    phases = [
        ("banner", 0, len(b"yanfs terminal\r\n"), TOHOST_OUTPUT),
        ("prompt", prompt_at, len(b"yanfs> "), TOHOST_OUTPUT),
        ("echo", echo_at, ok_at - echo_at, TOHOST_LINE_UNAVAILABLE),
        ("receipt", ok_at, len(b"OK create\r\n"), TOHOST_SHELL_FATAL),
        ("exit-receipt", ok_exit_at, len(b"OK exit\r\n"), TOHOST_SHELL_FATAL),
        ("exit", exit_at, len(exit_line), TOHOST_OUTPUT),
    ]
    # fault_at is a count of accepted bytes, hence each phase's last byte is
    # start + length, including the final LF rather than the preceding CR.
    stages = [(phase + "-" + edge, start + offset, code)
              for phase, start, length, code in phases
              for edge, offset in (("first", 1), ("mid", (length + 1) // 2),
                                   ("last", length))]
    seen = set()
    for label, at, expected_tohost in stages:
        if at in seen:
            continue
        seen.add(at)
        owner = "production-tx-" + label
        committed = at > ok_at
        result, status, disk, capture = run_production(
            driver, args, work, empty_image(), commands, "tx-" + label,
            "none", 0, at, baseline=baseline_path)
        try:
            values = classify_production(result, status, owner, {expected_tohost})
            if values["tx_fired"] != "1":
                raise OwnerFailure("%s: the tx fault never fired" % owner)
            if values["delivered"] != at:
                raise OwnerFailure("%s: delivered %d, expected %d"
                                   % (owner, values["delivered"], at))
            if values["out_length"] != at:
                raise OwnerFailure("%s: output length %d, expected %d"
                                   % (owner, values["out_length"], at))
            expected_image = (seeded_image([("kept.txt", b"x", 1)]) if committed
                              else empty_image())
            if disk.read_bytes() != expected_image:
                raise OwnerFailure("%s: the image after the fault does not match"
                                   " the independent oracle" % owner)
            results.append((owner, None))
        except OwnerFailure as error:
            results.append((owner, str(error)))
        except HarnessError as error:
            results.append((owner, "HARNESS: " + str(error)))

    # An IO fault on the create request: the Guest must stop with ERROR IO and
    # the following ls/exit must not run. The failed write left no committed
    # metadata, so the image stays the empty oracle.
    result, status, disk, capture = run_production(
        driver, args, work, empty_image(),
        b"create io.txt x\nls\nexit\n", "io", "io", 0, 0,
        expect_tohost=TOHOST_SHELL_FATAL)
    try:
        classify_production(result, status, "production-io",
                            {TOHOST_SHELL_FATAL})
        output = capture.read_bytes()
        if b"ERROR IO\r\n" not in output:
            raise OwnerFailure("production-io: ERROR IO is missing")
        if b"OK create\r\n" in output:
            raise OwnerFailure("production-io: a refused create printed success")
        if b"yanfs: exit\r\n" in output:
            raise OwnerFailure("production-io: a later command still ran")
        if disk.read_bytes() != empty_image():
            raise OwnerFailure("production-io: the image is not the empty oracle")
        results.append(("production-io", None))
    except OwnerFailure as error:
        results.append(("production-io", str(error)))
    except HarnessError as error:
        results.append(("production-io", "HARNESS: " + str(error)))

    # A wrong response tag after mount: the cat's first block read (mount uses
    # responses 1 and 2), so the mount succeeds and the failure is in the read.
    big = bytes((i * 7 + 3) & 0xFF for i in range(5000))
    result, status, disk, capture = run_production(
        driver, args, work, seeded_image([("big", big, 1)]),
        b"cat big\nexit\n", "protocol", "protocol", 3, 0,
        expect_tohost=TOHOST_SHELL_FATAL)
    try:
        values = classify_production(result, status, "production-protocol",
                                     {TOHOST_SHELL_FATAL})
        if values.get("protocol_fired") != "1":
            raise OwnerFailure("production-protocol: the tag was never corrupted")
        output = capture.read_bytes()
        if b"ERROR PROTOCOL\r\n" not in output:
            raise OwnerFailure("production-protocol: ERROR PROTOCOL is missing")
        if b"OK cat\r\n" in output:
            raise OwnerFailure("production-protocol: the failed cat printed OK")
        if disk.read_bytes() != seeded_image([("big", big, 1)]):
            raise OwnerFailure("production-protocol: a read-only failure changed"
                               " the image")
        results.append(("production-protocol", None))
    except OwnerFailure as error:
        results.append(("production-protocol", str(error)))
    except HarnessError as error:
        results.append(("production-protocol", "HARNESS: " + str(error)))

    # The same fault after the displayed prefix: block 1 is read in 16 chunks
    # (responses 3..18), so response 19 is the first block-2 read.
    result, status, disk, capture = run_production(
        driver, args, work, seeded_image([("big", big, 1)]),
        b"cat big\nexit\n", "protocol-prefix", "protocol", 19, 0,
        expect_tohost=TOHOST_SHELL_FATAL)
    try:
        values = classify_production(result, status, "production-protocol-prefix",
                                     {TOHOST_SHELL_FATAL})
        if values.get("protocol_fired") != "1":
            raise OwnerFailure("production-protocol-prefix: the tag was never"
                               " corrupted")
        output = capture.read_bytes()
        prefix = display(big[:BLOCK])
        if prefix not in output:
            raise OwnerFailure("production-protocol-prefix: the displayed prefix"
                               " differs from the independent display oracle")
        if b"OK cat\r\n" in output:
            raise OwnerFailure("production-protocol-prefix: a failed cat printed"
                               " its OK tail")
        if b"ERROR PROTOCOL\r\n" not in output:
            raise OwnerFailure("production-protocol-prefix: ERROR PROTOCOL is"
                               " missing")
        if disk.read_bytes() != seeded_image([("big", big, 1)]):
            raise OwnerFailure("production-protocol-prefix: a read-only failure"
                               " changed the image")
        results.append(("production-protocol-prefix", None))
    except OwnerFailure as error:
        results.append(("production-protocol-prefix", str(error)))
    except HarnessError as error:
        results.append(("production-protocol-prefix", "HARNESS: " + str(error)))


def main():
    parser = argparse.ArgumentParser(
        description="real UART window and production fault acceptance")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--guest", required=True)
    parser.add_argument("--only-fixtures", action="store_true",
                        help="run only the UART fixture windows (mutation builds)")
    parser.add_argument("--fixture-scenario",
                        choices=[name for name, _value in FIXTURE_SCENARIOS],
                        help="with --only-fixtures, run exactly this fixture")
    parser.add_argument("--sanitizers", action="store_true",
                        help="build the Host driver with ASan/UBSan")
    args = parser.parse_args()
    if args.fixture_scenario is not None and not args.only_fixtures:
        parser.error("--fixture-scenario requires --only-fixtures")

    source = args.source.resolve()
    missing = require_inputs(source, args)
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, flush=True)
        return 1
    if not os.path.isfile(args.guest):
        print(":FAIL: missing required input: --guest %s" % args.guest, flush=True)
        return 1
    if not tool_available(args.cc) or not tool_available(args.gcc):
        print("SKIP: no host or cross compiler (%s, %s)" % (args.cc, args.gcc),
              flush=True)
        return 77

    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = build_host(source, args.work, args.cc, sanitizers=args.sanitizers)
    except HarnessError as error:
        print(":HARNESS-ERROR: build: %s" % error, flush=True)
        return 2

    try:
        display_self_check()
    except AssertionError as error:
        print(":HARNESS-ERROR: display oracle: %s" % error, flush=True)
        return 2
    results = []
    baseline_ok = True
    # The fixture scenarios are independent: collect every verdict.
    if args.fixture_scenario is not None:
        selected = [(name, value) for name, value in FIXTURE_SCENARIOS
                    if name == args.fixture_scenario]
    else:
        selected = list(FIXTURE_SCENARIOS)
    for name, value in selected:
        try:
            elf = build_fixture(source, args.work, args.gcc, name, value)
            run_fixture(driver, elf, args.work, name)
            results.append(("fixture-" + name, None))
        except HarnessError as error:
            results.append(("fixture-" + name, "HARNESS: " + str(error)))
        except OwnerFailure as error:
            results.append(("fixture-" + name, str(error)))

    if not args.only_fixtures:
        try:
            case_production_faults(driver, args, args.work, results)
        except HarnessError as error:
            baseline_ok = False
            results.append(("production-harness", "HARNESS: " + str(error)))
        except OwnerFailure as error:
            baseline_ok = False
            results.append(("production-baseline", str(error)))

    # One record per case, all on stdout in order and flushed, so a mutation
    # runner that merges the streams sees the exact records before the footer.
    for name, failure in results:
        if failure is None:
            print("PASS %s" % name, flush=True)
        elif failure.startswith("HARNESS:"):
            print(":HARNESS-ERROR: %s: %s"
                  % (name, failure[len("HARNESS:"):].strip()), flush=True)
        else:
            print(":FAIL: %s" % failure, flush=True)

    passed = sum(1 for _, failure in results if failure is None)
    failed = sum(1 for _, failure in results
                 if failure is not None and not failure.startswith("HARNESS:"))
    harness = sum(1 for _, failure in results
                  if failure is not None and failure.startswith("HARNESS:"))
    print("terminal-runtime: %d passed, %d failed, %d harness errors (baseline %s)"
          % (passed, failed, harness, "ok" if baseline_ok else "failed"), flush=True)
    if not baseline_ok:
        return 2 if harness else 1
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
