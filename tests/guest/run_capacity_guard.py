#!/usr/bin/env python3
"""0036-R3: the terminal mounts and boots a v2 directory with more than 63 files.

PR2a's temporary CAPACITY guard is gone and the services allocate their tables
from the image's actual capacity, so an N=127 (D=2) image is an ordinary boot.
The real ``yan_run`` executor reports the Guest's healthy tohost word and the
console reaches the prompt and takes an exit command. The image is hashed
before and after to prove that mounting and exiting never wrote.
"""
import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

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
        description="0036-R3: N=127 mounts and boots normally")
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

    # N=127 is 64*2-1, so yan_mkfs writes a version 2 image with D=2 blocks.
    image = args.work / "capacity-127.img"
    format_image(args.mkfs, image, 127)
    before = sha256(image)
    result = run([args.run, "--image", args.guest, "--terminal",
                  "--disk-image", image, "--max-steps", MAX_STEPS],
                 stdin=b"exit\n")
    after = sha256(image)
    (args.work / "capacity-127.stdout").write_bytes(result.stdout)
    (args.work / "capacity-127.stderr").write_bytes(result.stderr)
    check(result.returncode == 0,
          "N=127 executor returned %d, expected PASS 0" % result.returncode)
    check(b"ERROR CAPACITY\r\n" not in result.stdout,
          "N=127 console still printed the removed CAPACITY error")
    check(b"yanfs terminal\r\n" in result.stdout,
          "N=127 console did not print the banner")
    check(b"yanfs> " in result.stdout,
          "N=127 console did not print the prompt")
    check(b"yanfs: exit\r\n" in result.stdout,
          "N=127 console did not take the exit command")
    check(before == after, "N=127 boot/exit changed the image")
    print("0036-R3 N=127 boot=ok", flush=True)

    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
