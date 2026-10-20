#!/usr/bin/env python3
"""0036-R2-AC4: a fixed write sequence on a v1 (--files 63) image produces the
same bytes as 41b83b5.

EXPECTED was generated once by building tests/yanfs_write_probe.c against
41b83b5's os/yanfs.c and os/yanfs.h (gcc -std=c17), running it on an image from
41b83b5's `yan_mkfs --image x.img --blocks 16`, and taking sha256sum. The fresh
image itself is pinned by 0036-R1-AC1 (7baad595...93fb75).
"""
import argparse
import hashlib
import os
import subprocess
import sys

EXPECTED = "b31403606b1e8a55e21d93d8025be6744010151fa259cffcf6d92e7101aa3ed5"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    image = os.path.join(args.work, "write-seq.img")
    if os.path.exists(image):
        os.remove(image)
    for command in ([args.mkfs, "--image", image, "--blocks", "16",
                     "--files", "63"], [args.probe, image]):
        proc = subprocess.run(command, capture_output=True, timeout=60)
        if proc.returncode != 0:
            print("FAIL %s exit %d: %s" % (command[0], proc.returncode,
                  proc.stderr.decode(errors="replace")))
            return 1
    with open(image, "rb") as handle:
        actual = hashlib.sha256(handle.read()).hexdigest()
    if actual != EXPECTED:
        print("FAIL 0036-R2-AC4 write sequence sha256 %s != %s" % (actual, EXPECTED))
        return 1
    print("PASS 0036-R2-AC4 write sequence sha256 %s" % actual)
    return 0


if __name__ == "__main__":
    sys.exit(main())
