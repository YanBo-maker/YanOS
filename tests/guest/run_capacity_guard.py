#!/usr/bin/env python3
# Temporary (0036 PR2a). PR2b removes the guard and APP_REASON_CAPACITY and flips this test to assert a normal mount for N>63.
"""0036 PR2a: the terminal refuses a directory with more than 63 entries.

The services still own 63-entry static arrays in this PR, so apps/yanfs_terminal
mounts N=127 (D=2) and then refuses it with APP_REASON_CAPACITY. The real
``yan_run`` executor reports the Guest's tohost word and the console carries the
CAPACITY token; the image is hashed before and after to prove the refusal never
wrote. An N=63 image must still boot, answer the prompt and exit normally.

Interface:
  --work DIR   directory for the two images and per-run logs (required)
  --run EXE    yan_run executor (required)
  --mkfs EXE   yan_mkfs formatter (required)
  --guest ELF  the production terminal ELF (required)

Exit codes: 0 pass, 1 a check failed, 2 a broken harness. Nothing here builds a
Guest or needs a cross compiler, so there is no skip code.
"""
import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

APP_FAIL_TOP = 0x71000000
APP_REASON_CAPACITY = 25
CAPACITY_CODE = APP_FAIL_TOP | APP_REASON_CAPACITY
BLOCKS = 16
MAX_STEPS = "50000000"
TIMEOUT = 300
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def run(argv, stdin=None):
    return subprocess.run([str(item) for item in argv],
                          input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=TIMEOUT)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def format_image(mkfs, path, files):
    if path.exists():
        path.unlink()
    made = run([mkfs, "--image", path, "--blocks", str(BLOCKS),
                "--files", str(files)])
    if made.returncode != 0:
        raise RuntimeError("mkfs --files %d failed: %s"
                           % (files, made.stderr.decode(errors="replace")))
    if not path.is_file():
        raise RuntimeError("mkfs did not create %s" % path)


def main():
    parser = argparse.ArgumentParser(
        description="0036 PR2a temporary capacity guard")
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--mkfs", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    args = parser.parse_args()

    missing = [label for label, path in
               (("executor", args.run), ("mkfs", args.mkfs),
                ("guest ELF", args.guest))
               if not Path(path).is_file()]
    if missing:
        print(":FAIL: missing input: " + ", ".join(missing))
        return 1
    args.work.mkdir(parents=True, exist_ok=True)

    # N=127 is 64*2-1, so yan_mkfs writes D=2 directory blocks.
    image = args.work / "capacity-127.img"
    format_image(args.mkfs, image, 127)
    before = sha256(image)
    result = run([args.run, "--image", args.guest, "--terminal",
                  "--disk-image", image, "--max-steps", MAX_STEPS], stdin=b"")
    after = sha256(image)
    (args.work / "capacity-127.stdout").write_bytes(result.stdout)
    (args.work / "capacity-127.stderr").write_bytes(result.stderr)
    check(result.returncode == 6,
          "N=127 executor returned %d, expected Guest failure 6"
          % result.returncode)
    check(("failure code %d" % CAPACITY_CODE).encode() in result.stderr,
          "N=127 tohost is not %d (0x%08x / APP_FAIL|%d)"
          % (CAPACITY_CODE, CAPACITY_CODE, APP_REASON_CAPACITY))
    check(b"ERROR CAPACITY\r\n" in result.stdout,
          "N=127 console did not print ERROR CAPACITY")
    check(before == after, "N=127 refusal changed the image")
    print("0036-PR2a-GUARD N=127 code=0x%08x token=CAPACITY sha=unchanged"
          % CAPACITY_CODE, flush=True)

    image63 = args.work / "capacity-63.img"
    format_image(args.mkfs, image63, 63)
    result = run([args.run, "--image", args.guest, "--terminal",
                  "--disk-image", image63, "--max-steps", MAX_STEPS],
                 stdin=b"exit\n")
    (args.work / "capacity-63.stdout").write_bytes(result.stdout)
    (args.work / "capacity-63.stderr").write_bytes(result.stderr)
    check(result.returncode == 0,
          "N=63 executor returned %d, expected PASS 0" % result.returncode)
    check(b"yanfs terminal\r\n" in result.stdout,
          "N=63 console did not print the banner")
    check(b"yanfs> " in result.stdout,
          "N=63 console did not print the prompt")
    check(b"ERROR CAPACITY\r\n" not in result.stdout,
          "N=63 console printed the CAPACITY error")
    check(b"yanfs: exit\r\n" in result.stdout,
          "N=63 console did not take the exit command")
    print("0036-PR2a-GUARD N=63 boot=ok", flush=True)

    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
