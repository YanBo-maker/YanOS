"""Independent whole-corpus tag qualification plus Unicode term reference."""
import random
import struct
import subprocess
import sys
from pathlib import Path
import note_metadata_reference as metadata
import search_terms_reference as terms


def payload(query, files, tags):
    raw = bytearray(struct.pack("<I", len(query)) + query)
    raw += struct.pack("<I", len(tags))
    for tag in tags:
        value = tag.encode()
        raw += struct.pack("<I", len(value)) + value
    raw += struct.pack("<I", len(files))
    for slot, name, data in files:
        encoded = name.encode("ascii")
        raw += struct.pack("<III", slot, len(encoded), len(data)) + encoded + data
    return raw


def check(executable, mode, query, files, tags, label):
    try:
        expected = metadata.filtered_reference(query, files, tags, terms.search_reference)
    except metadata.MetadataError:
        expected = None
    run = subprocess.run([executable, mode], input=payload(query, files, tags),
                         capture_output=True, timeout=30)
    if run.returncode:
        raise AssertionError((label, "probe/harness", run.returncode, run.stderr))
    rows, summary = [], None
    for line in run.stdout.decode("ascii").splitlines():
        fields = line.split("\t")
        if fields[0] == "M":
            rows.append((fields[1], int(fields[2]), int(fields[3]),
                         bool(int(fields[4])), bool(int(fields[5])), bytes.fromhex(fields[6])))
        elif fields[0] == "Q" and summary is None:
            summary = tuple(map(int, fields[1:]))
        else:
            raise AssertionError((label, "unexpected output", line))
    if summary is None or len(summary) != 7:
        raise AssertionError((label, "missing/invalid footer"))
    status, actual_mode, total, shown, skipped, reads, writes = summary
    if expected is None:
        assert status == 12 and not rows and writes == 0, (label, mode, summary, rows)
        assert (actual_mode,total,shown,skipped)==(1,0xdeadbeef,0xa5a5,0x5a5a), (label,"error published summary",summary)
        return
    wanted = [(r["name"], r["line"], r["score"], r["left_clipped"],
               r["right_clipped"], r["snippet"]) for r in expected["rows"]]
    assert status == 0 and writes == 0, (label, mode, summary)
    assert (total, shown, skipped) == (expected["total"], expected["shown"], expected["skipped"]), (label, summary, expected)
    assert rows == wanted, (label, mode, rows, wanted)
    want_mode = 0 if mode.startswith("index") and terms.index_fits_reference(files) else 1
    assert actual_mode == want_mode, (label, mode, summary, want_mode)


def head(tags, body):
    return b"@yan-note 1\n" + b"".join(b"tag: " + t.encode() + b"\n" for t in tags) + b"@end\n" + body


def fixtures():
    yield b"hit", [(0, "untagged", b"hit hit\n" * 30), (2, "eligible", head(["os"], b"hit\n"))], ["os"], "after-top20"
    yield b"hit", [(0, "ok", head(["os"], b"hit\n")), (62, "bad", b"@yan-note 1\nunknown: value\n@end\n")], ["os"], "last-slot-bad-head"
    yield b"absent", [(62, "bad", b"@yan-note 1\nunknown: value\n@end\n")], ["os"], "absent-keyword-still-bad-head"
    yield b"hit", [(0, "binary", b"@yan-note 9\n\0hit"), (2, "invalid", b"@yan-note 9\n\xff"), (5, "ok", head(["os"], b"hit\n"))], ["os"], "binary-before-bad-head"
    yield b"hit", [(0, "old", b"hit\n"), (2, "head", head(["os", "riscv"], b"hit\r\n"))], ["os", "os", "riscv"], "exact-and-duplicates"
    yield b"hit", [(0, "case", head(["OS"], b"hit\n")), (3, "norm", head(["café"], b"hit\n"))], ["café"], "exact-normalization"
    # Force key capacity, then posting capacity; fallback still filters all files.
    yield b"hit", [(0, "keys", b" ".join(("k%d" % i).encode() for i in range(8193))), (2, "ok", head(["os"], b"hit\n"))], ["os"], "limit-keys"
    yield b"hit", [(0, "postings", b"hit " * 65537), (2, "ok", head(["os"], b"hit\n"))], ["os"], "limit-postings"
    rng = random.Random(270008)
    vocabulary = ["hit", "CPU", "中断", "café", "🙂", "hit hit", "interruption"]
    for number in range(100):
        files = []
        for slot in sorted(rng.sample(range(12), rng.randrange(1, 6))):
            body = "".join(rng.choice(vocabulary) + rng.choice([" ", "\r\n", "\n", "\t", "-"]) for _ in range(rng.randrange(1, 35))).encode()
            tags = rng.sample(["os", "OS", "riscv", "中文", "café", "café"], rng.randrange(4))
            data = head(tags, body) if rng.randrange(4) else body
            if rng.randrange(10) == 0:
                data += rng.choice([b"\0", b"\xff", b"\xe4"])
            files.append((slot, "n%d" % slot, data))
        query = rng.choice(["hit", "CPU", "中断", "café", "🙂", "CPU 中断"]).encode()
        tags = rng.sample(["os", "OS", "riscv", "中文", "café", "café"], rng.randrange(1, 3))
        yield query, files, tags, "random-%d" % number


def main():
    executable, mode = sys.argv[1:3]
    for count, (query, files, tags, label) in enumerate(fixtures(), 1):
        check(executable, mode, query, files, tags, label)
    print("PASS: independent filtered rows/summary/score/snippet/flags/no-write", count, mode)


if __name__ == "__main__":
    main()
