#!/usr/bin/env python3
"""Shared 0036-R3 real-execution measurement used by the accounting and budget
tests.

Both tests must stand alone: each builds the Host driver in its own work
directory, formats its own image and runs the same supplied production ELF.
The observation comes from the Host-side `yan_arena_alloc` boundary trace in
tests/arena_trace.c, not from the Guest's final `yan_arena_used_bytes` word.
"""
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime  # noqa: E402

EXPECTED_CALLS = 11

# The exact declared inputs the production arena Guest and Host driver use.
# This deliberately does not add runtime.GUEST_SOURCES (the UART fixture
# terminal_wait_check.c), which these arena entries never compile.
ARENA_INPUTS = tuple(dict.fromkeys(
    list(runtime.HOST_SOURCES) + list(runtime.HOST_HEADERS) +
    list(runtime.PRODUCTION_SOURCES) + list(runtime.GUEST_HEADERS) +
    [runtime.GUEST_LD]))


def local_input_problem(source, guest, mkfs):
    """Return the local-input failure text, or None.

    Presence and priority only: all declared source/header files, the explicit
    Guest ELF and mkfs must exist before an external compiler can make the run
    skippable. File contents are left to the real build/run steps."""
    source = Path(source)
    missing = []
    if not source.is_dir():
        missing.append(str(source))
    else:
        missing.extend(str(source / relative) for relative in ARENA_INPUTS
                       if not (source / relative).is_file())
    for label, path in (("guest ELF", guest), ("mkfs", mkfs)):
        path = Path(path)
        if not path.is_file():
            missing.append("%s: %s" % (label, path))
    if not missing:
        return None
    return "missing required local input: " + ", ".join(missing)



def make_image(mkfs, path, files, blocks):
    """Format a fresh image and return its bytes."""
    path = Path(path)
    if path.exists():
        path.unlink()
    proc = subprocess.run([str(mkfs), "--image", str(path), "--blocks",
                           str(blocks), "--files", str(files)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("yan_mkfs failed: %s"
                           % proc.stderr.decode(errors="replace"))
    return path.read_bytes()


def trace(values, label):
    """Validate the driver's trace fields. Missing or incomplete observation is
    a harness error, never a zero-allocation result."""
    state = values.get("arena_trace_state", "missing")
    if state != "ok":
        raise runtime.HarnessError("%s: arena trace is %s" % (label, state))
    observed = {}
    for key in ("calls", "successes", "failures", "highwater"):
        raw = values.get("arena_trace_" + key)
        try:
            value = int(raw)
        except (TypeError, ValueError) as error:
            raise runtime.HarnessError(
                "%s: arena trace field %s is %r" % (label, key, raw)) from error
        if value < 0:
            raise runtime.HarnessError(
                "%s: arena trace field %s is negative" % (label, key))
        observed[key] = value
    return observed


def measure(driver, args, work, image, label, max_steps,
            expect_tohost=1):
    """Run one production ELF over `image` with the arena trace requested."""
    result, status, disk, capture = runtime.run_production(
        driver, args, work, image, b"exit\n", label, "none", 0, 0,
        expect_tohost=expect_tohost, max_steps=max_steps, trace_arena=True)
    values = runtime.classify_production(result, status, label,
                                         {expect_tohost})
    return trace(values, label), values, disk, capture.read_bytes()
