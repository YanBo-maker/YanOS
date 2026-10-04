#!/usr/bin/env python3
"""Terminal mutation gate: do the shell/line suites and the real runtime driver
actually detect planted defects?

Two families:

  native    tests/test_shell.c and tests/test_line.c (Unity). One exact,
            single-occurrence replacement per mutant; the mutant is *detected*
            only when the owning Unity test named in OWNING fails and no
            harness-level condition is present.

  runtime   the real driver of tests/guest/run_terminal_runtime.py over a
            mutated production ELF / fixture built from a copied tree. A
            detection needs the expected owner line from that script and no
            ":HARNESS-ERROR:" line.

The strict Unity classification (parse_unity / classify_run) is reused from
tests/run_yanfs_mutation.py by a no-pyc import, so the two gates agree on what a
detection is and neither script is edited.

The pure functions below (parse_runtime / classify_runtime / summary counters)
are exercised without a compiler by tests/test_terminal_mutation_gate.py.

Exit codes: 0 all detected, 1 survivor/wrong-owner, 2 harness error or a failed
baseline/control, 77 the external cc/gcc/Unity is missing. Everything in the
repository that is compiled is a hard failure (1), never 77 and never a build
error that counts as a detection.
"""
import argparse
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

STRICT_FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
MUTANT_FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic"]
BUILD_TIMEOUT = 180
RUN_TIMEOUT = 300
HARNESS_HINTS = (b"sanitizer", b"runtime error:", b"harness-error", b"fixture abort")


def _load(path, name):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_IMPORT_ERROR = None
try:
    _YANFS = _load(Path(__file__).with_name("run_yanfs_mutation.py"), "yanfs_mut_ref")
except Exception as error:  # a missing or broken shared helper is a hard failure
    _YANFS = None
    _IMPORT_ERROR = error
if _YANFS is not None:
    parse_unity = _YANFS.parse_unity
    classify_native_run = _YANFS.classify_run
    HarnessError = _YANFS.HarnessError
else:
    parse_unity = None
    classify_native_run = None

    class HarnessError(RuntimeError):
        """Stand-in so the module still imports when the helper is missing."""


# The runtime acceptance script is a repository input too: importing it gives
# the exact SOURCES/HEADERS it compiles, so a missing header is a hard 1.
_TERM_MODULE = None
_TERM_IMPORT_ERROR = None
try:
    _TERM_MODULE = _load(Path(__file__).with_name("guest") / "run_terminal_runtime.py",
                         "term_runtime_ref")
except Exception as error:
    _TERM_IMPORT_ERROR = error


# ------------------------------------------------------------- native suites

NATIVE_SUITES = {
    "shell": {"sources": ["tests/test_shell.c", "os/shell.c", "os/yanfs.c"],
              "includes": ["os"], "target": "test_shell"},
    "line": {"sources": ["tests/test_line.c", "os/line.c"],
             "includes": ["os"], "target": "test_line"},
}

# Every repository header the native TUs include (transitively, through the os
# headers). Missing any is a hard failure (1).
NATIVE_HEADERS = [
    "tests/test_shell.c", "tests/test_line.c",
    "os/shell.h", "os/yanfs.h", "os/line.h", "os/platform.h", "os/block.h",
    "os/task.h", "os/console.h", "os/terminal.h", "os/yanfs_block.h",
    "tests/run_yanfs_mutation.py",
]

# runtime production build recipe (the app plus the platform it links).
PRODUCTION_SOURCES = [
    "apps/yanfs_terminal/main.c", "os/trap_entry.S", "os/task_switch.S",
    "os/task.c", "os/block.c", "os/console.c", "os/yanfs.c",
    "os/yanfs_block.c", "os/shell.c", "os/line.c", "os/terminal.c",
    "os/memory.c",
]
CROSS_FLAGS = [
    "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany", "-nostdlib",
    "-nostartfiles", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-std=c17", "-static", "-O2",
]


# ----------------------------------------------------------- pure functions

SUMMARY = re.compile(
    r"^terminal-runtime: (\d+) passed, (\d+) failed, (\d+) harness errors"
    r" \(baseline (ok|failed)\)\s*$")
PASS_LINE = re.compile(r"^PASS (.+?)\s*$")
FAIL_LINE = re.compile(r"^:FAIL: ([^:]+):(.*)$")
HARNESS_LINE = re.compile(r"^:HARNESS-ERROR: (.+)$")


def parse_runtime(stdout, stderr=b""):
    """Strict decode of one run_terminal_runtime.py run.

    Records and the single footer must be on stdout, in order, and the footer
    must be the last line. Every non-empty stdout line must be a PASS, :FAIL: or
    :HARNESS-ERROR: record or the one footer; anything else (an IGNORE or other
    unknown record, a malformed footer, a late record) is rejected. Case names
    must be unique across all records and the footer counts must equal the
    records exactly. stderr may carry ordinary diagnostics, but any verdict
    record or footer there rejects the run: the executor will merge streams, so
    a record hidden on stderr must never be counted.
    """
    for raw in stderr.decode("utf-8", "replace").split("\n"):
        line = raw.rstrip("\r")
        if (PASS_LINE.match(line) or FAIL_LINE.match(line)
                or HARNESS_LINE.match(line) or SUMMARY.match(line)):
            return None
    lines = [line.rstrip("\r") for line in
             stdout.decode("utf-8", "replace").split("\n")]
    records = []
    footers = []
    for index, line in enumerate(lines):
        if line == "":
            continue
        match = SUMMARY.match(line)
        if match is not None:
            footers.append((index, match))
            continue
        match = PASS_LINE.match(line)
        if match is not None:
            records.append((index, "pass", match.group(1)))
            continue
        match = FAIL_LINE.match(line)
        if match is not None:
            records.append((index, "fail", match.group(1).strip()))
            continue
        match = HARNESS_LINE.match(line)
        if match is not None:
            records.append((index, "harness", match.group(1).strip()))
            continue
        return None
    if len(footers) != 1:
        return None
    footer_index, match = footers[0]
    if any(index > footer_index for index, _kind, _name in records):
        return None
    names = [name for _index, _kind, name in records]
    if len(set(names)) != len(names):
        return None
    passed = int(match.group(1))
    failed = int(match.group(2))
    harness = int(match.group(3))
    if (passed != sum(1 for _i, kind, _n in records if kind == "pass")
            or failed != sum(1 for _i, kind, _n in records if kind == "fail")
            or harness != sum(1 for _i, kind, _n in records if kind == "harness")):
        return None
    return {"passed": [n for _i, k, n in records if k == "pass"],
            "fails": [n for _i, k, n in records if k == "fail"],
            "fail_details": {m.group(1).strip(): m.group(2).strip()
                             for line in stdout.decode("utf-8", "replace").splitlines()
                             for m in [FAIL_LINE.match(line)] if m},
            "harness": [n for _i, k, n in records if k == "harness"],
            "baseline": match.group(4)}


def _runtime_output(records, harness=0, baseline="ok",
                    passes=None, fails=None, harness_count=None):
    """Build a well-formed run_terminal_runtime.py stdout for the controls."""
    lines = []
    for name, status in records:
        lines.append("PASS %s" % name if status == "PASS"
                     else ":FAIL: %s: bad" % name)
    for index in range(harness):
        lines.append(":HARNESS-ERROR: c%d: broke" % index)
    counted_pass = (sum(1 for _n, s in records if s == "PASS")
                    if passes is None else passes)
    counted_fail = (sum(1 for _n, s in records if s == "FAIL")
                    if fails is None else fails)
    counted_harness = harness if harness_count is None else harness_count
    lines.append("terminal-runtime: %d passed, %d failed, %d harness errors"
                 " (baseline %s)" % (counted_pass, counted_fail, counted_harness,
                                     baseline))
    return ("\n".join(lines) + "\n").encode()


def classify_runtime(returncode, stdout, stderr, owner, assertion=None):
    """Pure verdict for one runtime run.

    A detection needs exit 1, a strict stdout footer with unique records, the
    owner in the :FAIL: records and no harness condition. Exit 1 with no failure
    record, exit 0 with a FAIL record, a signal, a timeout, a sanitizer report,
    a malformed/late/duplicated record or a failed baseline is a harness error,
    never a detection.
    """
    text = stdout + b"\n" + stderr
    lowered = text.lower()
    if returncode is None or returncode < 0 or returncode >= 128:
        return "harness"
    if any(hint in lowered for hint in HARNESS_HINTS):
        return "harness"
    parsed = parse_runtime(stdout, stderr)
    if parsed is None:
        return "harness"
    if parsed["baseline"] != "ok" or parsed["harness"]:
        return "harness"
    if returncode == 2:
        return "harness"
    if returncode == 0:
        return "pass" if not parsed["fails"] else "harness"
    if returncode == 1:
        if not parsed["fails"]:
            return "harness"
        if owner in parsed["fails"]:
            if assertion is not None and assertion not in parsed["fail_details"][owner]:
                return "wrong-owner"
            return "owner-fail"
        return "wrong-owner"
    return "harness"


# --------------------------------------------------------------- mutants

# name -> (suite, owning Unity test). Each old/new must match exactly once.
NATIVE_MUTATIONS = [
    {
        "name": "io_named_as_protocol",
        "suite": "shell",
        "file": "os/shell.c",
        "old": '    case YAN_FS_IO: return "IO";\n',
        "new": '    case YAN_FS_IO: return "PROTOCOL";\n',
        "owner": "write_io_error_reports_io_and_faults",
    },
    {
        "name": "faulted_reported_ok",
        "suite": "shell",
        "file": "os/shell.c",
        "old": "    return fs_error_is_fatal(result) ? YAN_SHELL_FATAL : YAN_SHELL_OK;\n",
        "new": "    return YAN_SHELL_OK;\n",
        "owner": "faulted_filesystem_stops_output_only_commands",
    },
    {
        "name": "exit_reported_ok",
        "suite": "shell",
        "file": "os/shell.c",
        "old": '        return emit_ok(shell, "exit") ? YAN_SHELL_EXIT : YAN_SHELL_FATAL;\n',
        "new": '        return emit_ok(shell, "exit") ? YAN_SHELL_OK : YAN_SHELL_FATAL;\n',
        "owner": "exit_writes_ok_and_returns_exit",
    },
    {
        "name": "text_swallows_leading_space",
        "suite": "shell",
        "file": "os/shell.c",
        "old": "        *text_start = position + 1u;\n",
        "new": "        *text_start = position;\n",
        "owner": "write_preserves_extra_leading_and_trailing_spaces",
    },
    {
        "name": "cat_skips_a_chunk_byte",
        "suite": "shell",
        "file": "os/shell.c",
        "old": "            if (!cat_feed(shell, &stream, shell->scratch[i])) {\n",
        "new": "            if (!cat_feed(shell, &stream, (uint8_t)(shell->scratch[i] ^ 0xA5u))) {\n",
        "owner": "cat_of_a_large_file_reads_every_chunk",
    },
    {
        "name": "exit_ignores_output_failure",
        "suite": "shell",
        "file": "os/shell.c",
        "old": '        return emit_ok(shell, "exit") ? YAN_SHELL_EXIT : YAN_SHELL_FATAL;\n',
        "new": ('        (void)emit_ok(shell, "exit");\n'
                "        return YAN_SHELL_EXIT;\n"),
        "owner": "output_failure_on_the_exit_tail_is_fatal_not_exit",
    },
    {
        "name": "line_exit_left_masked",
        "suite": "line",
        "file": "os/line.c",
        "old": ("    line->io.arm_rx(line->io.context, false);\n"
                "    line->io.ack_rx(line->io.context);\n"),
        "new": ("    line->io.arm_rx(line->io.context, true);\n"
                "    line->io.ack_rx(line->io.context);\n"),
        "owner": "each_byte_is_masked_before_it_is_taken_and_rearmed_after",
    },
    {
        "name": "line_get0_does_not_rearm",
        "suite": "line",
        "file": "os/line.c",
        "old": ("            if (got == 0) {\n"
                "                /* The device reported ready but had nothing; re-arm and look\n"
                "                 * again from the top rather than spin here. */\n"
                "                line->io.arm_rx(line->io.context, true);\n"
                "                continue;\n"
                "            }\n"),
        "new": ("            if (got == 0) {\n"
                "                line->io.arm_rx(line->io.context, false);\n"
                "                continue;\n"
                "            }\n"),
        "owner": "ready_true_get_zero_rearms_then_reads",
    },
    {
        "name": "line_rearms_before_wait_masked",
        "suite": "line",
        "file": "os/line.c",
        "old": ("        line->io.arm_rx(line->io.context, true);\n"
                "        line->io.wait(line->io.context, line_predicate, line);\n"),
        "new": ("        line->io.arm_rx(line->io.context, false);\n"
                "        line->io.wait(line->io.context, line_predicate, line);\n"),
        "owner": "wait_path_delivers_bytes_without_empty_reads",
    },
    {
        "name": "line_control_not_rejected",
        "suite": "line",
        "file": "os/line.c",
        "old": "                } else if (byte < UINT8_C(0x20)) {\n",
        "new": "                } else if (byte < UINT8_C(0x00)) {\n",
        "owner": "nul_tab_escape_and_other_c0_reject_the_line",
    },
    {
        "name": "line_1024_accepted",
        "suite": "line",
        "file": "os/line.c",
        "old": "                } else if (line->length >= YAN_LINE_MAX) {\n",
        "new": "                } else if (line->length > YAN_LINE_MAX) {\n",
        "owner": "line_of_1024_bytes_is_too_long",
    },
    {
        "name": "create_replace_swapped",
        "suite": "shell",
        "file": "os/shell.c",
        "old": "    YanFsResult result = create\n",
        "new": "    YanFsResult result = !create\n",
        "owner": "create_duplicate_reports_exists_and_keeps_the_original",
    },
    {
        "name": "text_gets_implicit_lf",
        "suite": "shell",
        "file": "os/shell.c",
        "old": "    const uint8_t *text = text_length > 0u ? line + text_start : NULL;\n",
        "new": ("    uint8_t lf_text[1024];\n"
                "    uint32_t lf_length = text_length;\n"
                "    for (uint32_t i = 0; i < text_length; ++i) {\n"
                "        lf_text[i] = line[text_start + i];\n"
                "    }\n"
                "    if (lf_length < 1023u) {\n"
                "        lf_text[lf_length] = (uint8_t)'\\n';\n"
                "        ++lf_length;\n"
                "    }\n"
                "    const uint8_t *text = lf_length > 0u ? lf_text : NULL;\n"
                "    text_length = lf_length;\n"),
        "owner": "create_makes_a_new_file_with_the_exact_text",
    },
    {
        "name": "cat_ascii_controls_raw",
        "suite": "shell",
        "file": "os/shell.c",
        "old": "            return cat_emit_ascii(shell, byte);\n",
        "new": "            return shell_put_bytes(shell, &byte, 1u);\n",
        "owner": "cat_escapes_backslash_and_c0_and_del",
    },
]

RUNTIME_MUTATIONS = [
    {
        "name": "isr_reads_rxdata",
        "scope": "fixtures",
        "file": "os/task.c",
        "old": ("        yan_os_uart_set_rx_irq(0);\n"
                "        yan_os_uart_ack_rx();\n"),
        "new": ("        yan_os_uart_set_rx_irq(0);\n"
                "        {\n"
                "            uint8_t stolen = 0;\n"
                "            (void)yan_os_uart_get(&stolen);\n"
                "        }\n"
                "        yan_os_uart_ack_rx();\n"),
        "owner": "fixture-blocked",
        "assertion": "the UART handler consumed RXDATA while the source was in service",
    },
    {
        "name": "isr_missing_mask",
        "scope": "fixtures",
        "file": "os/task.c",
        "old": ("        yan_os_uart_set_rx_irq(0);\n"
                "        yan_os_uart_ack_rx();\n"),
        "new": "        yan_os_uart_ack_rx();\n",
        "owner": "fixture-blocked",
        "assertion": "completed UART service with RX_IRQ_ENABLE still set",
    },
    {
        "name": "wait_polls_instead_of_blocking",
        "scope": "fixtures",
        "file": "os/terminal.c",
        "old": ("    (void)context;\n"
                "    yan_os_task_wait(YAN_OS_EVENT_UART, predicate, predicate_context);\n"),
        "new": ("    (void)context;\n"
                "    while (predicate(predicate_context) == 0) {\n"
                "        yan_os_task_yield();\n"
                "    }\n"),
        "owner": "fixture-blocked",
        "assertion": "times before the blocked window (a yield/poll reader)",
    },
    {
        "name": "line_missing_rearm",
        "scope": "fixtures",
        "file": "os/line.c",
        "old": ("        line->io.arm_rx(line->io.context, true);\n"
                "        line->io.wait(line->io.context, line_predicate, line);\n"),
        "new": ("        line->io.arm_rx(line->io.context, false);\n"
                "        line->io.wait(line->io.context, line_predicate, line);\n"),
        "owner": "fixture-blocked",
        "assertion": "the reader is BLOCKED but RX_IRQ_ENABLE is clear",
    },
    {
        "name": "exit_output_failure_ignored",
        "scope": "full",
        "file": "apps/yanfs_terminal/main.c",
        "old": ('                if (!app_write("yanfs: exit\\r\\n")) {\n'
                "                    app_finish(APP_FAIL(APP_REASON_OUTPUT));\n"
                "                }\n"),
        "new": '                (void)app_write("yanfs: exit\\r\\n");\n',
        "owner": "production-tx-exit-last",
        "assertion": "a tx fault reached tohost PASS",
    },
    {
        "name": "fs_fatal_reported_exit",
        "scope": "full",
        "file": "os/shell.c",
        "old": "    return fs_error_is_fatal(result) ? YAN_SHELL_FATAL : YAN_SHELL_OK;\n",
        "new": "    return fs_error_is_fatal(result) ? YAN_SHELL_EXIT : YAN_SHELL_OK;\n",
        "owner": "production-io",
        "assertion": "tohost is 0x00000001 not the expected 0x7100000c",
    },
]

NO_EFFECT = {
    "name": "no-effect-control",
    "file": "os/shell.c",
    "old": "#include <stddef.h>\n",
    "new": "#include <stddef.h> /* no-effect control */\n",
    "owner": "exit_writes_ok_and_returns_exit",
}
WRONG_OWNER = {
    "name": "wrong-owner-control",
    "file": "os/shell.c",
    "old": '    case YAN_FS_IO: return "IO";\n',
    "new": '    case YAN_FS_IO: return "PROTOCOL";\n',
    "owner": "cat_of_a_large_file_reads_every_chunk",
}


def apply_replacement(path, old, new):
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise HarnessError("%s: replacement matched %d times, expected exactly 1"
                           % (path, count))
    path.write_text(text.replace(old, new, 1))


def copy_tree(source, unity, destination):
    for name in ("os", "tests", "src", "include", "tools", "apps"):
        origin = source / name
        if not origin.exists():
            raise HarnessError("repository tree is missing %s" % origin)
        shutil.copytree(origin, destination / name)
    (destination / "unity").mkdir()
    for item in sorted((unity / "src").iterdir()):
        if item.is_file():
            shutil.copy2(item, destination / "unity" / item.name)


def require_repo_inputs(source):
    needed = []
    for suite in NATIVE_SUITES.values():
        needed += suite["sources"]
    needed += NATIVE_HEADERS + PRODUCTION_SOURCES + [
        "os/guest.ld", "tests/guest/run_terminal_runtime.py",
        "tests/guest/terminal_wait_check.c", "tests/terminal_runtime_driver.c"]
    if _TERM_MODULE is not None:
        needed += list(_TERM_MODULE.HOST_SOURCES)
        needed += list(_TERM_MODULE.GUEST_SOURCES)
        needed += list(getattr(_TERM_MODULE, "HOST_HEADERS", []))
        needed += list(getattr(_TERM_MODULE, "GUEST_HEADERS", []))
    return [str(source / rel) for rel in needed if not (source / rel).is_file()]


def unity_object(tree, cc):
    out = tree / "bin"
    out.mkdir(exist_ok=True)
    obj = out / "unity.o"
    if obj.exists():
        return obj
    command = [cc, "-std=c17", "-I", str(tree / "unity"),
               "-c", str(tree / "unity/unity.c"), "-o", str(obj)]
    proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    if proc.returncode != 0:
        raise HarnessError("unity build failed:\n%s"
                           % proc.stderr.decode(errors="replace"))
    return obj


def compile_native(tree, cc, suite_name, strict):
    suite = NATIVE_SUITES[suite_name]
    flags = STRICT_FLAGS if strict else MUTANT_FLAGS
    out = tree / "bin"
    out.mkdir(exist_ok=True)
    obj = unity_object(tree, cc)
    target = out / suite["target"]
    includes = []
    for inc in suite["includes"] + ["unity"]:
        includes += ["-I", str(tree / inc)]
    command = [cc] + flags + includes + \
              [str(tree / rel) for rel in suite["sources"]] + [str(obj),
                                                               "-o", str(target)]
    proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    return proc, target, command


def run_binary(path):
    try:
        proc = subprocess.run([str(path)], capture_output=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, b"", b""
    return proc.returncode, proc.stdout, proc.stderr


def run_native(tree, cc, owner, strict=False):
    verdicts = {}
    blob = b""
    for name in NATIVE_SUITES:
        build, target, command = compile_native(tree, cc, name, strict)
        blob += ("\n# build %s: %s\n" % (name, " ".join(command))).encode()
        blob += build.stdout + build.stderr
        if build.returncode != 0:
            return "harness", blob
        rc, out, err = run_binary(target)
        blob += b"\n" + out + err
        verdicts[name] = classify_native_run(rc, out, err, owner)
    if "harness" in verdicts.values():
        return "harness", blob
    if "owner-fail" in verdicts.values():
        return "owner-fail", blob
    if "wrong-owner" in verdicts.values():
        return "wrong-owner", blob
    return "pass", blob


def build_production(tree, gcc, out_elf, strict=False):
    warnings = ["-Wall", "-Wextra", "-Wpedantic"] + (["-Werror"] if strict else [])
    command = [gcc] + CROSS_FLAGS + warnings + ["-I", str(tree / "os"),
                                     "-T", str(tree / "os/guest.ld")] + \
              [str(tree / rel) for rel in PRODUCTION_SOURCES] + ["-o", str(out_elf)]
    proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    return proc, command


def run_runtime(tree, cc, gcc, elf, work, only_fixtures=False,
                fixture_scenario=None):
    script = tree / "tests/guest/run_terminal_runtime.py"
    command = [sys.executable, "-B", str(script), "--source", str(tree),
               "--work", str(work), "--cc", cc, "--gcc", gcc,
               "--guest", str(elf)]
    if only_fixtures:
        command.append("--only-fixtures")
    if fixture_scenario:
        command += ["--fixture-scenario", fixture_scenario]
    try:
        # stderr is merged into stdout so the real record order is preserved;
        # the pure parser rejects any record after the footer.
        proc = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, b"", b""
    return proc.returncode, proc.stdout, b""


# ---------------------------------------------------------------- controls

def self_test_classifiers():
    """Permanent controls for the pure runtime classifier, run without a
    compiler by tests/test_terminal_mutation_gate.py and again here. Every
    malformed control is built from a literal record line, not by a helper that
    would map an unknown record to a verdict."""
    pass_out = _runtime_output([("a", "PASS"), ("b", "PASS")])
    owner_out = _runtime_output([("owner", "FAIL")])
    other_out = _runtime_output([("other", "FAIL")])
    cases = [
        ("pass", 0, pass_out, b"", "pass"),
        ("owner", 1, owner_out, b"", "owner-fail"),
        ("wrong", 1, other_out, b"", "wrong-owner"),
        ("exit1-no-fail", 1, _runtime_output([("a", "PASS")]), b"", "harness"),
        ("exit0-with-fail", 0, owner_out, b"", "harness"),
        ("empty", 0, b"", b"", "harness"),
        ("no-footer", 0, b"PASS a\n", b"", "harness"),
        ("double-footer", 1, owner_out + owner_out, b"", "harness"),
        ("count-wrong", 1, _runtime_output([("owner", "FAIL")], passes=9),
         b"", "harness"),
        ("late-pass", 1, owner_out + b"PASS late\n", b"", "harness"),
        ("late-fail", 1, pass_out + b":FAIL: late: x\n", b"", "harness"),
        ("duplicate-case", 1,
         _runtime_output([("owner", "FAIL"), ("owner", "FAIL")]), b"", "harness"),
        ("ignore-record", 1,
         b"a:IGNORE\nterminal-runtime: 1 passed, 0 failed, 0 harness errors"
         b" (baseline ok)\n", b"", "harness"),
        ("unknown-record", 1, b"WHATEVER x\n" + owner_out, b"", "harness"),
        ("malformed-footer", 1,
         b"PASS a\nterminal-runtime: two passed (baseline ok)\n", b"", "harness"),
        ("stderr-record", 1, owner_out, b"PASS sneaky\n", "harness"),
        ("stderr-note", 0, pass_out, b"ordinary diagnostic\n", "pass"),
        ("harness-beats-owner", 1,
         _runtime_output([("owner", "FAIL")], harness=1), b"", "harness"),
        ("baseline-failed", 1,
         _runtime_output([("owner", "FAIL")], baseline="failed"), b"", "harness"),
        ("sanitizer", 1, owner_out, b"runtime error: boom", "harness"),
        ("signal", 139, owner_out, b"", "harness"),
        ("timeout", None, owner_out, b"", "harness"),
    ]
    for label, rc, out, err, expected in cases:
        got = classify_runtime(rc, out, err, "owner")
        if got != expected:
            raise HarnessError("classify_runtime control '%s': got %s expected %s"
                               % (label, got, expected))


def main():
    parser = argparse.ArgumentParser(description="terminal mutation gate")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--unity", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--native-only", action="store_true")
    parser.add_argument("--runtime-only", action="store_true")
    args = parser.parse_args()

    if _IMPORT_ERROR is not None:
        print("FAIL: cannot import the shared Unity classifier"
              " (tests/run_yanfs_mutation.py): %r" % _IMPORT_ERROR,
              file=sys.stderr)
        return 1
    if _TERM_IMPORT_ERROR is not None:
        print("FAIL: cannot import tests/guest/run_terminal_runtime.py: %r"
              % _TERM_IMPORT_ERROR, file=sys.stderr)
        return 1
    if args.native_only and args.runtime_only:
        print("usage: --native-only and --runtime-only are mutually exclusive",
              file=sys.stderr)
        return 2

    try:
        self_test_classifiers()
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        return 2
    print("PASS mocked classify_runtime controls")

    if shutil.which(args.cc) is None:
        print("SKIP: external C compiler missing: %s" % args.cc)
        return 77
    unity = args.unity.resolve()
    if not (unity / "src/unity.c").is_file():
        print("SKIP: external Unity missing under %s/src" % unity)
        return 77
    if not args.native_only and shutil.which(args.gcc) is None:
        print("SKIP: external cross compiler missing: %s" % args.gcc)
        return 77

    source = args.source.resolve()
    missing = require_repo_inputs(source)
    if missing:
        print("FAIL the source under test is missing: %s" % ", ".join(missing),
              file=sys.stderr)
        return 1

    args.work.mkdir(parents=True, exist_ok=True)
    logdir = args.work / "terminal-mutation"
    logdir.mkdir(exist_ok=True)
    tree_root = Path(tempfile.mkdtemp(prefix="term-mut-", dir=args.work))
    harness = []
    survivors = []
    detected = 0
    try:
        copy_tree(source, unity, tree_root / "baseline")
        verdict, blob = run_native(tree_root / "baseline", args.cc, "", strict=True)
        (logdir / "baseline-native.log").write_bytes(blob)
        if verdict != "pass":
            print("HARNESS-ERROR: strict native baseline is %s" % verdict,
                  file=sys.stderr)
            return 2
        print("PASS native baseline builds strict and both suites pass")

        if not args.native_only:
            baseline_elf = tree_root / "baseline-production.elf"
            build, command = build_production(tree_root / "baseline", args.gcc,
                                              baseline_elf, strict=True)
            (logdir / "baseline-production-build.log").write_bytes(
                (" ".join(command) + "\n").encode() + build.stdout + build.stderr)
            if build.returncode != 0:
                print("HARNESS-ERROR: strict production baseline build failed",
                      file=sys.stderr)
                return 2
            rc, out, err = run_runtime(tree_root / "baseline", args.cc, args.gcc,
                                       baseline_elf, tree_root / "run-baseline")
            (logdir / "baseline-runtime.log").write_bytes(out + b"\n" + err)
            baseline_verdict = classify_runtime(rc, out, err, "")
            if rc != 0 or baseline_verdict != "pass":
                print("HARNESS-ERROR: full runtime baseline is %s (exit %s)"
                      % (baseline_verdict, rc), file=sys.stderr)
                return 2
            print("PASS full production runtime baseline (all cases)")

        for control, expected in ((NO_EFFECT, "pass"),
                                  (WRONG_OWNER, "wrong-owner")):
            tree = tree_root / ("control-" + control["name"])
            shutil.copytree(tree_root / "baseline", tree,
                            ignore=shutil.ignore_patterns("bin"))
            try:
                apply_replacement(tree / control["file"], control["old"],
                                  control["new"])
            except HarnessError as error:
                print("HARNESS-ERROR control %s: %s" % (control["name"], error),
                      file=sys.stderr)
                return 2
            verdict, blob = run_native(tree, args.cc, control["owner"])
            (logdir / ("control-" + control["name"] + ".log")).write_bytes(blob)
            if verdict != expected:
                print("HARNESS-ERROR control %s was %s, expected %s"
                      % (control["name"], verdict, expected), file=sys.stderr)
                return 2
            print("PASS control %s: %s" % (control["name"], verdict))

        if not args.runtime_only:
            for mutation in NATIVE_MUTATIONS:
                tree = tree_root / ("native-" + mutation["name"])
                shutil.copytree(tree_root / "baseline", tree,
                                ignore=shutil.ignore_patterns("bin"))
                try:
                    apply_replacement(tree / mutation["file"], mutation["old"],
                                      mutation["new"])
                except HarnessError as error:
                    harness.append((mutation["name"], str(error)))
                    print("HARNESS-ERROR %s: %s" % (mutation["name"], error))
                    continue
                verdict, blob = run_native(tree, args.cc, mutation["owner"])
                (logdir / (mutation["name"] + ".log")).write_bytes(blob)
                if verdict == "owner-fail":
                    detected += 1
                    print("PASS mutant %s: owner %s failed"
                          % (mutation["name"], mutation["owner"]))
                elif verdict == "harness":
                    harness.append((mutation["name"], "harness classification"))
                    print("HARNESS-ERROR %s: harness classification"
                          % mutation["name"])
                else:
                    survivors.append((mutation["name"], verdict))
                    print("FAIL mutant %s: %s" % (mutation["name"], verdict))

        if not args.native_only:
            for mutation in RUNTIME_MUTATIONS:
                tree = tree_root / ("runtime-" + mutation["name"])
                shutil.copytree(tree_root / "baseline", tree,
                                ignore=shutil.ignore_patterns("bin"))
                try:
                    apply_replacement(tree / mutation["file"], mutation["old"],
                                      mutation["new"])
                except HarnessError as error:
                    harness.append((mutation["name"], str(error)))
                    print("HARNESS-ERROR %s: %s" % (mutation["name"], error))
                    continue
                elf = tree / (mutation["name"] + ".elf")
                build, command = build_production(tree, args.gcc, elf)
                if build.returncode != 0:
                    (logdir / (mutation["name"] + ".log")).write_bytes(
                        (" ".join(command) + "\n").encode() + build.stderr)
                    harness.append((mutation["name"], "mutant production build failed"))
                    print("HARNESS-ERROR %s: mutant build failed" % mutation["name"])
                    continue
                fixture_scope = mutation.get("scope") == "fixtures"
                rc, out, err = run_runtime(
                    tree, args.cc, args.gcc, elf,
                    tree_root / ("run-" + mutation["name"]),
                    only_fixtures=fixture_scope,
                    fixture_scenario=("blocked" if fixture_scope else None))
                selected = "fixture-blocked" if fixture_scope else "all"
                (logdir / (mutation["name"] + ".log")).write_bytes(
                    ("# selected case: %s\n" % selected).encode()
                    + (" ".join(command) + "\n").encode() + out + b"\n" + err)
                verdict = classify_runtime(rc, out, err, mutation["owner"],
                                           mutation["assertion"])
                if verdict == "owner-fail":
                    detected += 1
                    print("PASS runtime mutant %s: owner %s failed"
                          % (mutation["name"], mutation["owner"]))
                elif verdict == "harness":
                    harness.append((mutation["name"], "harness classification"))
                    print("HARNESS-ERROR %s: harness classification"
                          % mutation["name"])
                else:
                    survivors.append((mutation["name"], verdict))
                    print("FAIL runtime mutant %s: %s"
                          % (mutation["name"], verdict))
    finally:
        shutil.rmtree(tree_root, ignore_errors=True)

    print("---")
    print("%d mutants detected, %d survivors/wrong-owner, %d harness errors;"
          " logs in %s" % (detected, len(survivors), len(harness), logdir))
    if harness:
        return 2
    if survivors:
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        sys.exit(2)
