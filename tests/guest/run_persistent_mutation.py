"""Rebuild isolated Host mutants; only completed Unity assertions count."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

EXPECTED_TESTS = 10
OWNING_ASSERTIONS = {
    "omit-write": "successful_write_is_visible_before_close",
    "wrong-block": "successful_write_is_visible_before_close",
    "omit-flush": "successful_write_is_visible_before_close",
    "ignore-flush-error": "flush_failure_never_acknowledges_success",
    "ignore-short-write": "short_write_is_device_failure",
    "ignore-short-read": "short_read_never_publishes_partial_payload",
    "protocol-false-success": "short_write_is_device_failure",
    "ignore-failed-backend": "failed_backend_rejects_reads_and_writes",
    "reopen-keeps-failure": "reopening_failed_backend_keeps_partial_write",
}


def verdict(result, owning_assertion=None):
    if result is None or not 0 <= result.returncode <= EXPECTED_TESTS or result.stderr:
        return "HARNESS-ERROR"
    summary = re.search(r"(\d+) Tests (\d+) Failures (\d+) Ignored", result.stdout)
    if not summary or int(summary[1]) != EXPECTED_TESTS or int(summary[3]) != 0:
        return "HARNESS-ERROR"
    failures = int(summary[2])
    if result.returncode == failures and failures > 0 and result.stdout.count(":FAIL:") == failures:
        failed_tests = re.findall(r":([^:\n]+):FAIL:", result.stdout)
        return "DETECTED" if owning_assertion in failed_tests else "HARNESS-ERROR"
    if result.returncode == 0 and failures == 0:
        return "SURVIVED"
    return "HARNESS-ERROR"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--unity", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    if not shutil.which(args.cc) or not (args.unity / "src/unity.c").is_file():
        print("SKIP: external C compiler or Unity missing")
        return 77
    source = args.source.resolve()
    test = source / "tests/test_host_disk.c"
    disk = (source / "tools/host_disk.c").read_text()
    block = (source / "tools/host_block.c").read_text()
    # Also exercise the classifier: crashes, stderr-only diagnostics, missing
    # verdicts and timeouts must never count as a killed mutant.
    for result in (None, subprocess.CompletedProcess([], -11, ":FAIL:", ""),
                   subprocess.CompletedProcess([], 1, "", "diagnostic"),
                   subprocess.CompletedProcess([], 2, ":FAIL:", "")):
        if verdict(result) != "HARNESS-ERROR":
            raise RuntimeError("mutation gate accepted an inconclusive result")
    claimed = "successful_write_is_visible_before_close"
    for test_name, expected in ((claimed, "DETECTED"), ("unrelated_test", "HARNESS-ERROR")):
        result = subprocess.CompletedProcess([], 1,
            f"fixture.c:1:{test_name}:FAIL: injected\n10 Tests 1 Failures 0 Ignored\n", "")
        if verdict(result, claimed) != expected:
            raise RuntimeError("mutation gate accepted the wrong owning assertion")
    cases = [
        ("baseline", "disk", "", "", "SURVIVED"),
        ("no-effect", "disk", "#include <limits.h>",
         "#include <limits.h>\n/* no behavioural change */", "SURVIVED"),
        ("omit-write", "disk", "fwrite(data, 1, length, disk->file) == length",
         "true", "DETECTED"),
        ("wrong-block", "disk", "fseek(disk->file, (long)offset, SEEK_SET)",
         "fseek(disk->file, (long)offset + 4096, SEEK_SET)", "DETECTED"),
        ("omit-flush", "disk", "fflush(disk->file) == 0", "true", "DETECTED"),
        ("ignore-flush-error", "disk", "fflush(disk->file) == 0",
         "(fflush(disk->file), true)", "DETECTED"),
        ("ignore-short-write", "disk", "fwrite(data, 1, length, disk->file) == length",
         "(fwrite(data, 1, length, disk->file), true)", "DETECTED"),
        ("ignore-short-read", "disk", "fread(data, 1, length, disk->file) == length",
         "fread(data, 1, length, disk->file) > 0", "DETECTED"),
        ("protocol-false-success", "block", "if (!success) {", "if (success) {", "DETECTED"),
        ("ignore-failed-backend", "disk", "&& !disk->failed &&", "&&", "DETECTED"),
        # Both lifecycle resets must retain the flag to form a real defect:
        # changing only one is equivalent because the other still clears it.
        ("reopen-keeps-failure", "disk",
         ("*disk = (YanHostDisk){0};", ".bytes = (uint64_t)bytes"),
         ("*disk = (YanHostDisk){.failed = disk->failed};",
          ".bytes = (uint64_t)bytes, .failed = disk->failed"), "DETECTED"),
    ]
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mut-", dir=args.work) as directory:
        work = Path(directory)
        for name, target, old, new, expected in cases:
            text = disk if target == "disk" else block
            if old:
                replacements = zip(old, new) if isinstance(old, tuple) else [(old, new)]
                for anchor, replacement in replacements:
                    if text.count(anchor) != 1:
                        raise RuntimeError(f"mutation anchor is not unique: {name}")
                    text = text.replace(anchor, replacement)
            disk_path, block_path = work / "host_disk.c", work / "host_block.c"
            disk_path.write_text(text if target == "disk" else disk)
            block_path.write_text(text if target == "block" else block)
            executable = work / name
            command = [args.cc, "-std=c17", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-I", str(source / "include"), "-I", str(source / "tools"),
                       "-I", str(args.unity / "src"), str(test), str(disk_path),
                       str(block_path), str(source / "src/ram.c"),
                       str(source / "src/transport.c"), str(args.unity / "src/unity.c"),
                       "-Wl,--wrap=fread", "-Wl,--wrap=fwrite",
                       "-Wl,--wrap=fflush", "-Wl,--wrap=fclose",
                       "-o", str(executable)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if built.returncode != 0:
                raise RuntimeError(f"{name} build failed: {built.stderr}")
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            (args.work / f"{name}.log").write_text(result.stdout + result.stderr)
            owner = OWNING_ASSERTIONS.get(name)
            actual = verdict(result, owner)
            print(f"{name}: {actual}" + (f" by {owner}" if actual == "DETECTED" else ""))
            if actual != expected:
                raise RuntimeError(f"{name}: expected {expected}, got {actual}")
    print("PASS: 9 detected, 0 survived; baseline and no-effect controls passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"HARNESS-ERROR: {error}", file=sys.stderr)
        sys.exit(1)
