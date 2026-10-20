#!/usr/bin/env python3
"""0035-R5-AC3: yan_run must refuse a disk image whose side lock is held."""

import fcntl
import os
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: note_write_lock_checks.py YAN_RUN YAN_MKFS GUEST_ELF",
              file=sys.stderr)
        return 2
    yan_run, yan_mkfs, guest_elf = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="yan-run-lock-") as directory:
        image = os.path.join(directory, "notes.img")
        made = subprocess.run(
            [yan_mkfs, "--image", image, "--blocks", "16"],
            capture_output=True, timeout=30)
        if made.returncode != 0:
            print(f"yan_mkfs failed: {made.stderr.decode(errors='replace')}",
                  file=sys.stderr)
            return 1
        lock_path = os.path.realpath(image) + ".lock"
        with open(lock_path, "w", encoding="utf-8") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            ran = subprocess.run(
                [yan_run, "--image", guest_elf, "--disk-image", image,
                 "--max-steps", "1"],
                capture_output=True, timeout=30)
        if ran.returncode == 0:
            print("yan_run ran the Guest while another process held the lock",
                  file=sys.stderr)
            return 1
        if b"is locked by another writer" not in ran.stderr:
            print(f"missing lock diagnostic: {ran.stderr.decode(errors='replace')}",
                  file=sys.stderr)
            return 1
        if not os.path.exists(lock_path):
            print("the side lock disappeared", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
