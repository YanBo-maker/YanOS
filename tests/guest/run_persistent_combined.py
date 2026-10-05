"""Combined acceptance: cooperative runtime + block protocol + file backend.

What this runs
--------------
Two simulator processes on one image file:

  1. the *writer* image (tests/guest/persistent_combined.c, PERSIST_ROLE=0)
     issues block I/O from a task through os/block.c, waits for the write with
     yan_os_task_wait() -- once on the already-happened path and once inside a
     real BLOCKED window built from the response ring's flow control -- is woken
     by the device interrupt and exits through tohost;
  2. the host independently compares the image bytes it produced: the target
     block, every other block (the seed) and the file length;
  3. the *reader* image (PERSIST_ROLE=1) runs in a *fresh* yan_run process on
     the same image and reads the written blocks back byte for byte;
  4. the host checks that the reader did not change a single byte.

The guest's own evidence lines are required, not just exit code 0: a run that
reached tohost without printing the window and read-back lines did not exercise
what this case exists for, and is reported as a harness error.

--mutation rebuilds the same case against planted defects. A defect counts as
detected only when the check its table entry declares is the check that failed,
and every entry names that check explicitly:

  * a Guest assertion has to carry the `pcombined: FAIL code=... phase=...`
    marker *and* the executor's own report of the same failure code, so the
    marker and the reason the process stopped cannot disagree. Exit code 6 on
    its own is not evidence - the runtime's own panic exits 6 too - and neither
    is the bare category "the reader failed";
  * a Host defect names the exact host check it must trip: `host-image-length`,
    `host-image-bytes` or `reader-changed-image`.

A wrong code, a wrong phase, a missing marker, a crash, a timeout, a signal
death and a sanitizer report are harness errors and are never detections. Two
controls are part of the gate itself: a no-behaviour-change Host mutant must
survive, and a wait that hangs must be classified INCONCLUSIVE rather than
detected. The verdict rules are protected by negative controls in
tests/guest/test_persistent_combined_gate.py, which this script runs before it
builds anything (and which is registered with CTest on its own).

Exit codes: 0 pass, 1 an assertion failed or a source/executor this case needs is
missing from the repository, 2 usage or a broken harness, 77 the cross toolchain
this machine does not have. 77 is the *only* skip: the images, the runtime, the
framing layer, the tool and the harness are all the thing under test.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

BLOCK_BYTES = 4096
BLOCKS = 8
IMAGE_BYTES = BLOCK_BYTES * BLOCKS
WRITER = 0
READER = 1
WRITER_MAX_STEPS = "3000000"
READER_MAX_STEPS = "1000000"

# The negative controls for this gate's verdict rules. They are part of the
# sources under test: the gate refuses to run without them and runs them before
# it builds anything.
CONTROL_TESTS = "test_persistent_combined_gate.py"

# The failure codes tests/guest/persistent_combined.c defines. Duplicated here
# on purpose: the gate compares the *Guest's* report against the assertion it
# expects to own each defect, so a mismatch is a failure of the gate, not
# something to paper over.
FAIL_DRAIN_PAYLOAD = 0x50000011
FAIL_NO_WINDOW = 0x50000013
FAIL_POLLED_WHILE_BLOCKED = 0x50000014

# What a detection is allowed to be. Every mutant names exactly one of these;
# there is deliberately no "any host check" or "any reader error" category, and
# a Guest assertion is only accepted with the code and phase the mutant
# predicted (see guest_assertion and verdict_for).
WRITER_ASSERTION = "writer-assertion"    # the writer Guest's verified FAIL marker
READER_ASSERTION = "reader-assertion"    # the reader Guest's verified FAIL marker
HOST_IMAGE_BYTES = "host-image-bytes"    # the image bytes differ from the model
HOST_IMAGE_LENGTH = "host-image-length"  # the image is no longer 8 blocks
READER_CHANGED_IMAGE = "reader-changed-image"  # the reader wrote to the image

DETECTED = "DETECTED"
SURVIVED = "SURVIVED"
INCONCLUSIVE = "INCONCLUSIVE"


def guest_detection(source, code, phase):
    """A detection that must come from a Guest assertion. The code and phase are
    part of the expectation, not a hint: verdict_for compares both."""
    return {"verdict": DETECTED, "source": source, "code": code, "phase": phase}


def host_detection(source):
    """A detection that must come from one named host check. A Guest assertion is
    a different source, so it can never satisfy this expectation."""
    return {"verdict": DETECTED, "source": source, "code": None, "phase": None}


def control(verdict):
    """A control mutant's expected verdict, with no detection source."""
    return {"verdict": verdict, "source": None, "code": None, "phase": None}


# The detection each planted defect must produce, declared here once and used by
# both the mutation runner and the negative controls. wrong-block shifts reads
# and writes together, so the Guest reads back its own shifted write and passes;
# only the host's independent byte comparison owns that defect.
FAKE_WAIT_EXPECT = guest_detection(WRITER_ASSERTION, FAIL_POLLED_WHILE_BLOCKED, 10)
YIELD_ONCE_EXPECT = guest_detection(WRITER_ASSERTION, FAIL_NO_WINDOW, 10)
PURE_HANG_EXPECT = control(INCONCLUSIVE)
OMIT_WRITE_EXPECT = guest_detection(WRITER_ASSERTION, FAIL_DRAIN_PAYLOAD, 20)
WRONG_BLOCK_EXPECT = host_detection(HOST_IMAGE_BYTES)
NO_EFFECT_EXPECT = control(SURVIVED)


# --------------------------------------------------------------- the image

# The byte patterns the Guest writes and compares, reproduced independently:
# every one is a function of the index inside its 4096-byte block.
def _pattern(scale, step, base):
    return bytes((i * scale + (i >> 8) * step + base) & 255 for i in range(BLOCK_BYTES))


def seed_block():
    return _pattern(7, 11, 93)


def pattern_a():
    return _pattern(17, 29, 41)


def pattern_c():
    return _pattern(17, 29, 13)


def seeded_image():
    """The image before the writer runs: every block carries the seed."""
    return seed_block() * BLOCKS


def expected_image():
    """The image after the writer runs: block 1 is pattern A, block 3 pattern C,
    every other block is still the seed. This is the host's independent model of
    what the two Guest roles are supposed to have done."""
    image = bytearray(seeded_image())
    image[1 * BLOCK_BYTES:2 * BLOCK_BYTES] = pattern_a()
    image[3 * BLOCK_BYTES:4 * BLOCK_BYTES] = pattern_c()
    return bytes(image)


def check_image(path, expect_written=True):
    """The host's own verdict on the file, with no Guest involved. Returns None
    when the image matches, otherwise a (source, problem) pair that names which
    of the two checks failed, so a mutant's expectation can tell them apart."""
    data = path.read_bytes()
    if len(data) != IMAGE_BYTES:
        return (HOST_IMAGE_LENGTH,
                f"image length {len(data)} != {IMAGE_BYTES}: a write changed the file size")
    wanted = expected_image() if expect_written else seeded_image()
    if data != wanted:
        for index in range(IMAGE_BYTES):
            if data[index] != wanted[index]:
                lba, offset = divmod(index, BLOCK_BYTES)
                return (HOST_IMAGE_BYTES,
                        f"image differs from the host's expectation at block {lba} "
                        f"offset {offset}: image {data[index]}, expected {wanted[index]}")
    return None


def check_the_check(directory):
    """The checks above are only evidence if they can fail, and if each failure
    names itself: this feeds check_image a flipped byte and a short copy and
    requires the matching source back instead of any AssertionError at all."""
    good = directory / "control-good.img"
    good.write_bytes(expected_image())
    if check_image(good) is not None:
        raise AssertionError("the host byte comparison rejected the expected image")
    flipped = directory / "control-flipped.img"
    data = bytearray(expected_image())
    data[2 * BLOCK_BYTES + 17] ^= 0x5a
    flipped.write_bytes(data)
    problem = check_image(flipped)
    if problem is None or problem[0] != HOST_IMAGE_BYTES:
        raise AssertionError(f"the byte comparison did not name a byte mismatch: {problem}")
    short = directory / "control-short.img"
    short.write_bytes(expected_image()[:-1])
    problem = check_image(short)
    if problem is None or problem[0] != HOST_IMAGE_LENGTH:
        raise AssertionError(f"the length check did not name a length change: {problem}")


# ------------------------------------------------------------- process facts

def run_process(command, timeout=120):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                              stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"process timed out: {command}") from error


def classify(result):
    """How a Guest run ended. Only ASSERTION is a verdict about a defect; a
    crash, a sanitizer report, a step-limit stop and a broken tool are all
    harness errors."""
    if result is None or result.returncode is None:
        return "HARNESS-ERROR"
    blob = f"{result.stdout}\n{result.stderr}"
    if "Sanitizer" in blob or "runtime error:" in blob:
        return "HARNESS-ERROR"
    if result.returncode < 0:
        return "HARNESS-ERROR"  # signal death
    if result.returncode == 0:
        return "PASS"
    if result.returncode == 6:
        return "ASSERTION"
    if result.returncode == 4:
        return "INCONCLUSIVE"
    return "HARNESS-ERROR"


def classify_self_test():
    """The classifier is the gate's own judge, so its rules are exercised on
    synthetic results before any mutant is run."""
    def done(returncode, stdout="", stderr=""):
        return subprocess.CompletedProcess([], returncode, stdout, stderr)

    cases = [
        (None, "HARNESS-ERROR"),                     # no result at all
        (done(-11), "HARNESS-ERROR"),                # signal death
        (done(-9), "HARNESS-ERROR"),                 # killed
        (done(1, "", "diagnostic"), "HARNESS-ERROR"),  # broken harness
        (done(2, ":FAIL:", ""), "HARNESS-ERROR"),    # usage error
        (done(5, "", ""), "HARNESS-ERROR"),          # host error
        (done(0, "", ""), "PASS"),
        (done(6, "", ""), "ASSERTION"),
        (done(4, "", ""), "INCONCLUSIVE"),
        (done(1, "", "ERROR: AddressSanitizer: heap-buffer-overflow"), "HARNESS-ERROR"),
        (done(0, "", "Sanitizer: undefined-behavior"), "HARNESS-ERROR"),
    ]
    for result, want in cases:
        got = classify(result)
        if got != want:
            raise RuntimeError(f"classifier self-test: {want} expected, got {got}")


def plain(text):
    """The Guest console writes CRLF; every pattern below is anchored on $."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def guest_verdict(text):
    """The Guest's own report: (code, phase), or (None, None)."""
    match = re.search(r"^pcombined: FAIL code=0x([0-9a-f]+) phase=(\d+)$", plain(text), re.M)
    if match is None:
        return None, None
    return int(match.group(1), 16), int(match.group(2))


def executor_failure_code(text):
    """The failure code the executor says the Guest halted with, or None. The
    executor prints this to stderr before it exits 6, so the marker and the
    process' own exit reason can be compared."""
    match = re.search(r"^yan_run: the Guest reported failure code (\d+)(?:\s|$)",
                      plain(text), re.M)
    return int(match.group(1)) if match is not None else None


def guest_assertion(result):
    """The Guest's assertion, verified end to end. Exit code 6 alone is not
    evidence: the runtime's own panic also exits 6, and a marker the executor
    does not report (or reports differently) means the two disagree about why
    the run stopped. Returns (code, phase, problem); exactly one is set."""
    if result.returncode != 6:
        return None, None, f"exit {result.returncode} is not a Guest assertion"
    text = result.stdout + result.stderr
    if "Sanitizer" in text or "runtime error:" in text:
        return None, None, "a sanitizer report is a harness error, never an assertion"
    code, phase = guest_verdict(text)
    if code is None:
        return None, None, ("exit 6 without a pcombined FAIL marker: a runtime panic or a "
                            "broken console is not an assertion")
    reported = executor_failure_code(result.stderr)
    if reported is None:
        return None, None, (f"the executor did not report a failure code for marker "
                            f"0x{code:08x}: the marker has no exit reason to agree with")
    if reported != code:
        return None, None, (f"the marker says 0x{code:08x} but the executor reported "
                            f"{reported} (0x{reported:08x}): marker and exit reason "
                            "disagree")
    return code, phase, None


# --------------------------------------------------------------- the sources

def require_sources(source):
    guest = source / "tests/guest"
    needed = [guest / "persistent_combined.c", guest / "guest_lib.c", guest / "link.ld",
              guest / CONTROL_TESTS,
              source / "os/trap_entry.S", source / "os/task_switch.S", source / "os/task.c",
              source / "os/task.h", source / "os/block.c", source / "os/block.h",
              source / "os/platform.h", source / "os/console.c",
              source / "tools/host_disk.c", source / "tools/host_disk.h",
              source / "tools/host_block.c", source / "tools/yan_run.c"]
    for path in needed:
        if not path.is_file():
            print(f"FAIL the source under test is missing: {path}", file=sys.stderr)
            return False
    return True


def build_guest(args, source, out, role, task_c=None, werror=True):
    """Builds one role. The strict warning bar applies to the real sources; a
    planted copy of os/task.c may legitimately leave a helper unused once its
    wait body is replaced, so the mutants are built without -Werror."""
    guest = source / "tests/guest"
    command = [args.gcc, "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany",
               "-static", "-nostdlib", "-nostartfiles", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-O2", "-Wall", "-Wextra",
               *(("-Werror",) if werror else ()), "-I", str(guest), "-I", str(source / "os"),
               f"-DPERSIST_ROLE={role}", f"-DPERSIST_DISK_BLOCKS={BLOCKS}",
               "-T", str(guest / "link.ld"),
               str(source / "os/trap_entry.S"), str(task_c or source / "os/task.c"),
               str(source / "os/task_switch.S"), str(source / "os/block.c"),
               str(source / "os/console.c"), str(guest / "guest_lib.c"),
               str(guest / "persistent_combined.c"),
               "-Wl,--build-id=none", "-o", str(out)]
    built = run_process(command, timeout=120)
    if built.returncode != 0:
        raise RuntimeError(f"guest build failed ({out.name}):\n{built.stdout}{built.stderr}")
    return out


def build_executor(args, source, out, host_disk_c):
    """Rebuilds yan_run with one Host source replaced, so a Host-side defect can
    be planted without touching the tree. -O1 with the same warning bar as the
    project build."""
    sources = sorted((source / "src").glob("*.c"))
    command = [args.cc, "-std=c17", "-O1", "-Wall", "-Wextra", "-Werror",
               "-I", str(source / "include"), "-I", str(source / "tools"),
               str(source / "tools/yan_run.c"), str(source / "tools/host_file.c"),
               str(source / "tools/host_terminal.c"), str(source / "tools/host_block.c"),
               str(host_disk_c), *map(str, sources), "-o", str(out)]
    built = run_process(command, timeout=180)
    if built.returncode != 0:
        raise RuntimeError(f"executor build failed ({out.name}):\n{built.stdout}{built.stderr}")
    return out


# ------------------------------------------------------------- the case

WRITER_EVIDENCE = (
    ("capacity", r"^pcombined: capacity blocks=8$"),
    ("fast path", r"^pcombined: fast write lba=3 bytes=4096 polls=1 peer-ticks=0$"),
    ("backpressure", r"^pcombined: backpressure used=(\d+) free=(\d+) reply=16$"),
    ("window 0 header",
     r"^pcombined: window round=0 kind=write budget=64 ticks=64 polls=1 frames=171$"),
    ("window 0 awaited",
     r"^pcombined: window round=0 awaited=write lba=1 status=ok count=1$"),
    ("window 1 header",
     r"^pcombined: window round=1 kind=read budget=32 ticks=32 polls=1 frames=1$"),
    ("window 1 read-back",
     r"^pcombined: window round=1 read-back lba=1 peer-read=pattern-a match=1$"),
    ("verdict", r"^pcombined: PASS$"),
)

READER_EVIDENCE = (
    ("capacity", r"^pcombined: reader capacity blocks=8$"),
    ("target block", r"^pcombined: reader lba=1 bytes=4096 match=1 pattern=pattern-a$"),
    ("fast-path block", r"^pcombined: reader lba=3 bytes=4096 match=1 pattern=pattern-c$"),
    ("neighbour block", r"^pcombined: reader lba=2 bytes=4096 match=1 pattern=seed$"),
    ("verdict", r"^pcombined: reader PASS$"),
)


def evidence_missing(text, wanted):
    body = plain(text)
    missing = []
    for label, pattern in wanted:
        if re.search(pattern, body, re.M) is None:
            missing.append(label)
    return missing


def check_backpressure_claim(text):
    """The Guest prints the ring state it blocked on; the host checks the
    arithmetic of that claim instead of trusting the printed numbers."""
    match = re.search(r"^pcombined: backpressure used=(\d+) free=(\d+) reply=16$",
                      plain(text), re.M)
    if match is None:
        return "the back-pressure line is missing"
    used, free = int(match.group(1)), int(match.group(2))
    if used + free != 8191:
        return f"the ring accounting is inconsistent: used={used} free={free} (8191 total)"
    if used + 16 <= 8191:
        return f"the awaited 16-byte reply still fitted: used={used}"
    return None


def run_combined(executor, writer_elf, reader_elf, image, max_steps=WRITER_MAX_STEPS):
    """The whole acceptance, through the production executor. Returns a dict:
    kind (PASS / DETECTED / INCONCLUSIVE / HARNESS-ERROR), detection (the named
    check that failed, or None), code, phase, host_check, reader_kind, evidence
    problems. Both Guest roles go through the same guest_assertion, so a run
    only becomes a detection with its marker and the executor's matching exit
    reason."""
    outcome = {"kind": "HARNESS-ERROR", "detection": None, "code": None, "phase": None,
               "host_check": "not-run", "reader_kind": "not-run",
               "missing": [], "problem": ""}
    writer = run_process([executor, "--image", str(writer_elf), "--disk-image", str(image),
                          "--terminal", "--max-steps", max_steps], timeout=300)
    outcome["writer"] = writer.stdout + writer.stderr
    outcome["kind"] = classify(writer)
    if outcome["kind"] == "ASSERTION":
        code, phase, problem = guest_assertion(writer)
        if problem is not None:
            outcome["kind"] = "HARNESS-ERROR"
            outcome["problem"] = problem
            return outcome
        outcome["kind"] = DETECTED
        outcome["code"], outcome["phase"] = code, phase
        outcome["detection"] = guest_detection(WRITER_ASSERTION, code, phase)
        return outcome
    if outcome["kind"] != "PASS":
        outcome["problem"] = writer.stderr.strip()[-400:]
        return outcome

    outcome["missing"] = evidence_missing(outcome["writer"], WRITER_EVIDENCE)
    if outcome["missing"]:
        outcome["kind"] = "HARNESS-ERROR"
        outcome["problem"] = "the writer reached tohost but its evidence is missing"
        return outcome
    claim = check_backpressure_claim(outcome["writer"])
    if claim is not None:
        outcome["kind"] = "HARNESS-ERROR"
        outcome["problem"] = claim
        return outcome

    problem = check_image(image, expect_written=True)
    if problem is not None:
        source, detail = problem
        outcome["host_check"] = f"{source}: {detail}"
        outcome["kind"] = DETECTED
        outcome["detection"] = host_detection(source)
        outcome["problem"] = detail
        return outcome
    outcome["host_check"] = "pass"

    before = image.read_bytes()
    reader = run_process([executor, "--image", str(reader_elf), "--disk-image", str(image),
                          "--terminal", "--max-steps", READER_MAX_STEPS], timeout=300)
    outcome["reader"] = reader.stdout + reader.stderr
    outcome["reader_kind"] = classify(reader)
    if outcome["reader_kind"] == "ASSERTION":
        code, phase, problem = guest_assertion(reader)
        if problem is not None:
            outcome["kind"] = "HARNESS-ERROR"
            outcome["problem"] = problem
            return outcome
        outcome["kind"] = DETECTED
        outcome["code"], outcome["phase"] = code, phase
        outcome["detection"] = guest_detection(READER_ASSERTION, code, phase)
        return outcome
    if outcome["reader_kind"] != "PASS":
        outcome["kind"] = "HARNESS-ERROR"
        outcome["problem"] = reader.stderr.strip()[-400:]
        return outcome
    missing = evidence_missing(outcome["reader"], READER_EVIDENCE)
    if missing:
        outcome["kind"] = "HARNESS-ERROR"
        outcome["problem"] = f"the reader reached tohost but its evidence is missing: {missing}"
        return outcome
    if image.read_bytes() != before:
        outcome["kind"] = DETECTED
        outcome["detection"] = host_detection(READER_CHANGED_IMAGE)
        outcome["problem"] = "the reader process changed the image"
        return outcome
    outcome["kind"] = "PASS"
    return outcome


# --------------------------------------------------------------- mutations

def replace_wait(text, replacement, name):
    start = text.index("void yan_os_task_wait(YanOsEvent event, "
                       "int (*predicate)(void *), void *context)\n{")
    end = text.index("\n}\n", start) + len("\n}\n")
    mutated = text[:start] + replacement + text[end:]
    if replacement not in mutated or "waiters[event] = current;" in mutated:
        raise RuntimeError(f"the {name} mutation was not applied")
    return mutated


WAIT_FAKE = """void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)
{
    (void)event;
    while (!predicate(context)) {
        yan_os_task_yield(); /* MUTANT: yield + busy-wait instead of blocking */
    }
}
"""

WAIT_YIELD_ONCE = """void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)
{
    (void)event;
    if (!predicate(context)) {
        yan_os_task_yield(); /* MUTANT: one yield, then return anyway */
    }
}
"""

WAIT_HANG = """void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)
{
    (void)event;
    (void)predicate;
    (void)context;
    for (volatile uint32_t spin = 0;; ++spin) { } /* CONTROL: never returns */
}
"""

# name, replacement, declared detection/control, note
GUEST_MUTANTS = (
    ("fake-wait", WAIT_FAKE, FAKE_WAIT_EXPECT,
     "wait = yield + busy-wait passes every functional check; the blocked window's "
     "poll count is the assertion that owns it (code 0x50000014 at phase 10)"),
    ("yield-once", WAIT_YIELD_ONCE, YIELD_ONCE_EXPECT,
     "one yield then return: wait returns before the window ran, so the window's "
     "own no-window assertion owns it (code 0x50000013 at phase 10)"),
    ("pure-hang", WAIT_HANG, PURE_HANG_EXPECT,
     "CONTROL: a wait that never returns must be inconclusive, never a detection"),
)

HOST_MUTANTS = (
    ("omit-write", "fwrite(data, 1, length, disk->file) == length", "true",
     OMIT_WRITE_EXPECT,
     "the file never receives the bytes; round 1's drain reads block 1 back, finds "
     "the seed instead of pattern A, and the writer's own payload assertion fires "
     "(code 0x50000011 at phase 20)"),
    ("wrong-block", "fseek(disk->file, (long)offset, SEEK_SET)",
     "fseek(disk->file, (long)offset + 4096, SEEK_SET)",
     WRONG_BLOCK_EXPECT,
     "reads and writes both land one block late, so the Guest reads its own shifted "
     "write back and exits 0; only the host's independent byte comparison sees "
     "pattern A in block 2 and pattern C in block 4 instead of blocks 1 and 3"),
    ("no-effect", "#include \"host_disk.h\"",
     "#include \"host_disk.h\"\n/* MUTANT: no behavioural change */",
     NO_EFFECT_EXPECT,
     "CONTROL: a change that alters nothing must survive"),
)


def describe_expectation(expected):
    """One line a reviewer can compare the run's actual evidence against."""
    if expected["verdict"] != DETECTED:
        return expected["verdict"]
    if expected["code"] is None:
        return f"{DETECTED} by {expected['source']}"
    return (f"{DETECTED} by {expected['source']} code=0x{expected['code']:08x} "
            f"phase={expected['phase']}")


def describe_detection(detection):
    """The named check that actually failed, in the same shape."""
    if detection is None:
        return "none"
    if detection["code"] is None:
        return detection["source"]
    return (f"{detection['source']} code=0x{detection['code']:08x} "
            f"phase={detection['phase']}")


def verdict_for(expected, outcome):
    """Classifies one mutant run against the detection its table entry declares.
    Only the declared check counts: another Guest assertion, another host check,
    a panic or a crash is a harness error, never a detection."""
    actual = outcome["detection"]
    if expected["verdict"] == SURVIVED:
        return SURVIVED if outcome["kind"] == "PASS" else f"HARNESS-ERROR ({outcome['kind']})"
    if expected["verdict"] == INCONCLUSIVE:
        return INCONCLUSIVE if outcome["kind"] == "INCONCLUSIVE" \
            else f"HARNESS-ERROR ({outcome['kind']})"
    if actual is None:
        detail = outcome["problem"] or outcome["kind"]
        return f"HARNESS-ERROR ({outcome['kind']}: {detail})"
    if actual["source"] != expected["source"]:
        return (f"HARNESS-ERROR (detected by {describe_detection(actual)}, expected "
                f"{describe_expectation(expected)})")
    if expected["code"] is not None and actual["code"] != expected["code"]:
        return (f"HARNESS-ERROR ({actual['source']} code 0x{actual['code']:08x}, "
                f"expected 0x{expected['code']:08x})")
    if expected["phase"] is not None and actual["phase"] != expected["phase"]:
        return (f"HARNESS-ERROR ({actual['source']} right code, phase {actual['phase']}, "
                f"expected {expected['phase']})")
    return DETECTED


def mutation_verdict(mutant_kind, expected_kind, expected_code, expected_phase, outcome):
    """The entry point review harnesses were written against. It builds the same
    explicit expectation the tables above use and maps 'ASSERTION' to the writer
    Guest's own verified marker - never to 'any assertion', so a reader failure
    cannot be waved through by the mutant's category. New callers use
    verdict_for(...) with the table entry's declared expectation."""
    if expected_kind == "ASSERTION":
        expected = guest_detection(WRITER_ASSERTION, expected_code, expected_phase)
    else:
        expected = control(expected_kind)
    return verdict_for(expected, outcome)


def owning_evidence(text):
    """The lines a reviewer needs to see for a detection: the window the case
    measured and the assertion that fired."""
    lines = [line for line in plain(text).splitlines()
             if line.startswith("pcombined: window round=0 ")
             or line.startswith("pcombined: window round=1 ")
             or line.startswith("pcombined: backpressure ")
             or line.startswith("pcombined: FAIL ")
             or line.startswith("pcombined: PASS")]
    return lines


def run_mutations(args, source, work, writer_elf, reader_elf, executor, logs):
    failures = 0
    scratch = work / "mutants"
    scratch.mkdir(parents=True, exist_ok=True)
    case = work / "mutation-case"
    case.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)

    print("== controls ==")
    classify_self_test()
    print("classifier self-test: crash, stderr-only, no-verdict and sanitizer results "
          "are HARNESS-ERROR")
    check_the_check(case)
    print("host byte comparison: rejects a flipped byte and a short image")

    # A control that must NOT be reported as a detection: a build that fails.
    broken = scratch / "build-error.c"
    broken.write_text("#error planted build failure (mutation-gate control)\n"
                      + (source / "os/task.c").read_text())
    try:
        build_guest(args, source, scratch / "build-error.elf", WRITER, task_c=broken)
    except RuntimeError:
        print("PASS control build-error: a mutant that does not compile is "
              "HARNESS-ERROR, not a detection")
    else:
        print("FAIL control build-error: a source that does not compile was accepted")
        failures += 1

    print("== baseline ==")
    image = case / "baseline.img"
    image.write_bytes(seeded_image())
    baseline = run_combined(executor, writer_elf, reader_elf, image)
    if baseline["kind"] != "PASS":
        print(f"FAIL the unmutated tree does not pass the case: {baseline['kind']} "
              f"{baseline['problem']}")
        return 1
    print("PASS baseline: the unmutated tree passes the writer, the host's byte "
          "comparison and the fresh reader")

    print("== guest mutants (os/task.c) ==")
    task_text = (source / "os/task.c").read_text()
    for name, replacement, expected, note in GUEST_MUTANTS:
        try:
            mutated = replace_wait(task_text, replacement, name)
        except RuntimeError as error:
            print(f"HARNESS mutation {name}: {error}")
            failures += 1
            continue
        copy = scratch / f"{name}.c"
        copy.write_text(mutated)
        try:
            elf = build_guest(args, source, scratch / f"{name}.elf", WRITER, task_c=copy,
                              werror=False)
        except RuntimeError as error:
            print(f"HARNESS mutation {name} does not build:\n{error}")
            failures += 1
            continue
        image = case / f"{name}.img"
        image.write_bytes(seeded_image())
        try:
            outcome = run_combined(executor, elf, reader_elf, image)
        except RuntimeError as error:
            print(f"HARNESS mutation {name}: {error}")
            failures += 1
            continue
        verdict = verdict_for(expected, outcome)
        want = expected["verdict"]
        print(f"mutation {name}: {verdict}")
        print(f"    declared: {describe_expectation(expected)}")
        print(f"    planted: {note}")
        print(f"    kind={outcome['kind']} detected-by={describe_detection(outcome['detection'])}")
        if outcome.get("problem"):
            print(f"    {outcome['problem']}")
        for line in owning_evidence(outcome.get("writer", "")):
            print(f"    {line}")
        (logs / f"{name}.log").write_text(outcome.get("writer", ""))
        if verdict != want:
            print(f"FAIL mutation {name}: expected {want}, got {verdict}")
            failures += 1

    print("== host mutants (tools/host_disk.c) ==")
    disk_text = (source / "tools/host_disk.c").read_text()
    for name, old, new, expected, note in HOST_MUTANTS:
        if old is not None:
            if disk_text.count(old) != 1:
                print(f"HARNESS mutation {name}: anchor is not unique")
                failures += 1
                continue
            mutated = disk_text.replace(old, new)
        else:
            mutated = disk_text
        copy = scratch / f"{name}.c"
        copy.write_text(mutated)
        try:
            mutant_run = build_executor(args, source, scratch / f"{name}-yan_run", copy)
        except RuntimeError as error:
            print(f"HARNESS mutation {name} does not build:\n{error}")
            failures += 1
            continue
        image = case / f"{name}.img"
        image.write_bytes(seeded_image())
        try:
            outcome = run_combined(mutant_run, writer_elf, reader_elf, image)
        except RuntimeError as error:
            print(f"HARNESS mutation {name}: {error}")
            failures += 1
            continue
        verdict = verdict_for(expected, outcome)
        print(f"mutation {name}: {verdict}")
        print(f"    declared: {describe_expectation(expected)}")
        print(f"    planted: {note}")
        print(f"    kind={outcome['kind']} detected-by={describe_detection(outcome['detection'])} "
              f"host-check={outcome['host_check']} reader={outcome['reader_kind']}")
        if outcome.get("problem"):
            print(f"    {outcome['problem']}")
        for line in owning_evidence(outcome.get("writer", "")):
            print(f"    {line}")
        (logs / f"{name}.log").write_text(outcome.get("writer", "") +
                                          outcome.get("reader", ""))
        want = expected["verdict"]
        if verdict != want:
            print(f"FAIL mutation {name}: expected {want}, got {verdict}")
            failures += 1

    if failures != 0:
        print(f"FAIL the mutation gate reported {failures} broken step(s)")
        return 1
    print("PASS every planted defect was caught by the check it declared, "
          "and both controls behaved")
    return 0


# ------------------------------------------------------------------- main

def run_negative_controls():
    """The gate's own negative controls, run as a unittest file before anything
    is built. They are also registered with CTest separately; running them here
    means this case cannot report PASS while its verdict rules would accept a
    reader panic or a mismatched expectation."""
    controls = Path(__file__).resolve().with_name(CONTROL_TESTS)
    if not controls.is_file():
        print(f"FAIL the gate's negative controls are missing: {controls}", file=sys.stderr)
        return 1
    # Discovery, not "run the file": a control file that forgets unittest.main()
    # would otherwise exit 0 without running a single case. -B keeps the source
    # tree free of bytecode caches.
    result = run_process([sys.executable, "-B", "-m", "unittest", "discover",
                          "-s", str(controls.parent), "-p", controls.name, "-v"],
                         timeout=120)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        print("HARNESS-ERROR: the gate's own negative controls failed", file=sys.stderr)
        return 2
    if "Ran 0 tests" in result.stderr:
        print("HARNESS-ERROR: the gate's negative controls ran no cases", file=sys.stderr)
        return 2
    print(f"PASS the gate's negative controls ({controls.name}): a reader panic, an "
          "unfitting expectation and a marker/executor disagreement are never detections")
    return 0

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--cc", default=None)
    parser.add_argument("--mutation", action="store_true")
    args = parser.parse_args()

    if not shutil.which(args.gcc):
        print("SKIP: missing external cross compiler")
        return 77
    source = args.source.resolve()
    work = args.work.resolve()
    if not require_sources(source):
        return 1
    # The verdict rules are checked before anything is built: a gate that cannot
    # tell a reader panic from a detection must not report PASS.
    controls = run_negative_controls()
    if controls != 0:
        return controls
    if not Path(args.run).is_file():
        print(f"FAIL the executor under test is missing: {args.run}")
        return 1
    # A named executor without --disk-image is a tool regression, not a missing
    # dependency: 77 here would let it skip the case and leave the suite green.
    helper = run_process([args.run, "--help"], timeout=30)
    if "--disk-image" not in (helper.stdout + helper.stderr):
        print(f"FAIL the executor under test has no --disk-image option: {args.run}")
        return 1
    if args.mutation:
        if not args.cc or not shutil.which(args.cc):
            print("SKIP: missing Host C compiler for the mutation gate")
            return 77

    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="case-", dir=work) as directory:
        case = Path(directory)
        writer_elf = build_guest(args, source, case / "writer.elf", WRITER)
        reader_elf = build_guest(args, source, case / "reader.elf", READER)
        if args.mutation:
            return run_mutations(args, source, case, writer_elf, reader_elf, args.run,
                                 logs=work / "mutation-logs")

        image = case / "disk.img"
        image.write_bytes(seeded_image())
        check_the_check(case)
        outcome = run_combined(args.run, writer_elf, reader_elf, image)
        if outcome["kind"] == DETECTED:
            print(f":FAIL: the unmutated tree was caught by "
                  f"{describe_detection(outcome['detection'])}: {outcome['problem']}")
            print(outcome.get("writer", ""))
            print(outcome.get("reader", ""))
            return 1
        if outcome["kind"] != "PASS":
            print(f"HARNESS-ERROR: {outcome['kind']}: {outcome['problem']}")
            print(outcome.get("writer", ""))
            return 2
        print("PASS the writer exited with the window and read-back evidence:")
        for line in outcome["writer"].splitlines():
            if line.startswith("pcombined:"):
                print(f"    {line}")
        print(f"PASS host image check: {IMAGE_BYTES} bytes, target block 1 = pattern A, "
              "block 3 = pattern C, every other block still the seed")
        print("PASS the fresh reader process read all 4096-byte blocks back and left "
              "the image unchanged:")
        for line in outcome["reader"].splitlines():
            if line.startswith("pcombined:"):
                print(f"    {line}")
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f":FAIL: {error}", file=sys.stderr)
        sys.exit(1)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"HARNESS-ERROR: {error}", file=sys.stderr)
        sys.exit(2)
