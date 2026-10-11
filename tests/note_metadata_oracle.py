"""Compare a native header probe with an independent calendar/UTF-8 model."""
import importlib.util
import json
import random
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "note_reference", Path(__file__).with_name("note_metadata_reference.py"))
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


def expectations(data):
    try:
        info = reference.parse_header(data)
    except reference.MetadataError:
        return {"result": "METADATA"}
    return {
        "result": "OK", "present": info["present"],
        "header_length": info["header_length"],
        "title_hex": (info["title"].encode().hex()
                      if info["title"] is not None else None),
        "created": info["created"], "modified": info["modified"],
        "tags_hex": [tag.encode().hex() for tag in info["tags"]],
        "links": info["links"],
        "encoded_hex": (reference.canonical_header(info).hex()
                        if info["present"] else None),
    }


def fixtures():
    yield b"", "empty"
    yield b"@yan-not", "partial-magic"
    yield b"\xef\xbb\xbf@yan-note 1\n@end\n", "BOM-old-file"
    yield b"body\n@yan-note 1\nunknown: value\n", "body-marker"
    yield b"@yan-note 1\n@end\n\0\xff", "binary-body"
    for raw in (b"@yan-note", b"@yan-note 2\n@end\n",
                b"@yan-note 1\r\n@end\n", b"@yan-note 1\n@end",
                b"@yan-note 1\n" + b"x" * 4090):
        yield raw, "bad-marker/end"
    # Current locked head rule rejects FEFF anywhere in a field; body is opaque.
    for key in (b"title", b"tag"):
        for value in (b"\xef\xbb\xbfabc", b"ab\xef\xbb\xbfcd", b"abc\xef\xbb\xbf"):
            yield b"@yan-note 1\n" + key + b": " + value + b"\n@end\n", "locked-FEFF-field"
    yield b"@yan-note 1\n@end\n\xef\xbb\xbfbody", "FEFF-body-opaque"
    yield b"@yan-note 1\n\xef\xbb\xbftag: os\n@end\n", "FEFF-before-key"
    fields = [b"title:  title with spaces  ", b"created: 2000-02-29",
              b"modified: 9999-12-31", b"tag: os", b"tag: OS",
              "tag: café".encode(), "tag: café".encode(), b"link: irq.md"]
    rng = random.Random(270007)
    for number in range(200):
        chosen = rng.sample(fields, rng.randrange(len(fields) + 1))
        rng.shuffle(chosen)
        data = b"@yan-note 1\n" + b"".join(x + b"\n" for x in chosen)
        data += b"@end\n" + rng.choice([b"", b"text\r\n", b"\0\xff"])
        yield data, "field-order-%d" % number
    for date in (b"0000-01-01", b"0001-01-01", b"1900-02-29",
                 b"2000-02-29", b"2024-02-29", b"2025-02-29",
                 b"2026-04-31", b"2026-00-01", b"2026-13-01",
                 b"2026-01-00", b"2026-1-01", b"9999-12-31"):
        yield b"@yan-note 1\ncreated: " + date + b"\n@end\n", "calendar"
    for size in (0, 1, 63, 64, 256, 257):
        for field in (b"title", b"tag", b"link"):
            yield (b"@yan-note 1\n" + field + b": " + b"a" * size +
                   b"\n@end\n"), "field-byte-boundary"
    for codepoint in sorted(reference.WHITE_SPACE | set(range(0xA0))):
        if 0xD800 <= codepoint <= 0xDFFF:
            continue
        value = b"a" + chr(codepoint).encode("utf-8") + b"b"
        yield b"@yan-note 1\ntag: " + value + b"\n@end\n", "tag-space/control"
        yield b"@yan-note 1\ntitle: " + value + b"\n@end\n", "title-space/control"
    for value in (b"\xff", b"\xc0\xaf", b"\xed\xa0\x80",
                  b"\xf4\x90\x80\x80", b"\xe4\xb8"):
        yield b"@yan-note 1\ntitle: " + value + b"\n@end\n", "bad-UTF8"
    for field in (b"title: a", b"tag: os", b"link: irq.md",
                  b"created: 2026-10-07", b"modified: 2026-10-07"):
        yield (b"@yan-note 1\n" + field + b"\n" + field + b"\n@end\n"), "duplicate"
    for count in (16, 17):
        for key in (b"tag", b"link"):
            yield (b"@yan-note 1\n" + b"".join(
                key + b": n" + str(n).encode() + b"\n" for n in range(count)) +
                b"@end\n"), "list-count-boundary"
    for field in (b"title:a", b"Title: a", b"unknown: a", b"tag: os os",
                  b"link: .", b"link: ..", b"link: x/y", b"", b"# comment"):
        yield b"@yan-note 1\n" + field + b"\n@end\n", "field-syntax"


def main():
    executable = sys.argv[1]
    count = 0
    for data, label in fixtures():
        # Only the bounded prefix is offered to the pure parser. The rest is
        # deliberately absent, just as the streaming service stops at its head.
        prefix = data[:4096]
        run = subprocess.run([executable], input=struct.pack("<I", len(prefix)) + prefix,
                             capture_output=True, timeout=20)
        if run.returncode:
            raise AssertionError((label, "probe/harness failure", run.returncode,
                                  run.stderr.decode(errors="replace")))
        observed = json.loads(run.stdout)
        wanted = expectations(prefix)
        if observed != wanted:
            raise AssertionError((label, data, observed, wanted))
        count += 1
    print("PASS: independent note metadata oracle", count, "cases")


if __name__ == "__main__":
    main()
