#!/usr/bin/env python3
"""Independent 0033 Task B oracle.

Builds YanFS images with tests/semantic_fixture.py, derives blocks and references
from raw bytes with tests/block_references_reference.py, computes source digests
and page ranges/lines in Python, then invokes the native probe and compares the
complete output. A wrong byte, range, line or source digest fails the run.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from block_references_reference import BlockError, scan, target  # noqa: E402
from semantic_fixture import image, note  # noqa: E402

CHECKS = 0


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(message)


def source_digest(files):
    digest = hashlib.sha256()
    present = [(slot, name, data) for slot, item in enumerate(files)
               if item is not None for name, data in [item]]
    for slot, name, data in sorted(present, key=lambda item: item[1].encode("ascii")):
        encoded = name.encode("ascii")
        digest.update(struct.pack("<I", slot))
        digest.update(struct.pack("<I", len(encoded)))
        digest.update(encoded)
        digest.update(struct.pack("<I", len(data)))
        digest.update(data)
    return digest.hexdigest()


def write_image(directory, filename, files):
    path = os.path.join(directory, filename)
    with open(path, "wb") as handle:
        handle.write(image(files))
    return path


def probe(executable, image_path, *arguments):
    result = subprocess.run([executable, image_path, *arguments],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise AssertionError("probe exit %d: %s" % (result.returncode,
                                                   result.stderr))
    return result.stdout.splitlines()


def expect_error(lines, token):
    check(len(lines) == 1 and lines[0] == "ERROR " + token,
          "expected ERROR %s, got %r" % (token, lines))


def blocks_for(files, name):
    return scan(files[name])[0]


def list_expected(files, name):
    blocks = blocks_for(files, name)
    return [dict(block) for block in blocks]


def check_list(executable, path, files, name, digest):
    try:
        expected = list_expected(files, name)
    except BlockError as error:
        expect_error(probe(executable, path, "list", name), error.token)
        return
    lines = probe(executable, path, "list", name)
    check(lines[0] == "LIST %d %d" % (len(expected), len(expected)),
          "list header %r" % lines[0])
    check(len(lines) == 1 + len(expected) + 1, "list line count %r" % lines)
    for line, block in zip(lines[1:], expected):
        fields = line.split()
        check(fields[0] == "BLOCK", "list row %r" % line)
        check(fields[1] == block["id"], "list id %r" % line)
        numbers = [int(value) for value in fields[2:]]
        check(numbers == [block["marker"], block["first"], block["last"],
                          block["start"], block["end"]],
              "list fields %r for %r" % (numbers, block))
    check(lines[-1] == "SOURCE " + digest, "list source digest %r" % lines[-1])


def page_expected(data, block, offset):
    start, end = block["start"], block["end"]
    length = end - start
    if offset > length:
        return ("ERROR", "INVALID")
    position = start + offset
    if offset == length:
        return dict(block_start=start, block_end=end,
                    block_first=block["first"], block_last=block["last"],
                    page_start=position, page_end=position,
                    page_first=0, page_last=0, text=b"", complete=True,
                    next_offset=0)
    if data[position] & 0xC0 == 0x80:
        return ("ERROR", "INVALID")
    if position > start and data[position - 1:position + 1] == b"\r\n":
        return ("ERROR", "INVALID")
    page_end = min(position + 65536, end)
    if page_end < end:
        while data[page_end] & 0xC0 == 0x80:
            page_end -= 1
        if data[page_end - 1:page_end + 1] == b"\r\n":
            page_end -= 1
    if page_end <= position:
        return ("ERROR", "INVALID")
    first_line = block["first"] + data.count(b"\n", start, position)
    last_line = first_line + data.count(b"\n", position, page_end - 1)
    return dict(block_start=start, block_end=end,
                block_first=block["first"], block_last=block["last"],
                page_start=position, page_end=page_end,
                page_first=first_line, page_last=last_line,
                text=data[position:page_end], complete=page_end == end,
                next_offset=0 if page_end == end else page_end - start)


def check_read_lines(lines, expected):
    check(len(lines) == 3, "read line count %r" % lines)
    fields = lines[0].split()
    check(fields[0] == "READ", "read header %r" % lines[0])
    numbers = [int(value) for value in fields[1:]]
    check(numbers == [expected["block_start"], expected["block_end"],
                      expected["block_first"], expected["block_last"],
                      expected["page_start"], expected["page_end"],
                      expected["page_first"], expected["page_last"],
                      1 if expected["complete"] else 0,
                      expected["next_offset"], len(expected["text"])],
          "read fields %r for %r" % (numbers, expected))
    text_fields = lines[1].split()
    check(text_fields[0] == "TEXT", "read text tag %r" % lines[1])
    encoded = "" if text_fields[1] == "-" else text_fields[1]
    check(bytes.fromhex(encoded) == expected["text"],
          "read text mismatch for offset %d" % expected["page_start"])


def check_read(executable, path, files, name, identity, offset, digest):
    block = next(block for block in blocks_for(files, name)
                 if block["id"] == identity)
    expected = page_expected(files[name], block, offset)
    lines = probe(executable, path, "read", name, identity, str(offset))
    if isinstance(expected, tuple):
        expect_error(lines, expected[1])
        return None
    check_read_lines(lines, expected)
    check(lines[2] == "SOURCE " + digest, "read source digest %r" % lines[2])
    return expected


def check_pages(executable, path, files, name, identity, digest):
    block = next(block for block in blocks_for(files, name)
                 if block["id"] == identity)
    data = files[name]
    offset = 0
    joined = b""
    pages = 0
    while True:
        expected = check_read(executable, path, files, name, identity, offset,
                              digest)
        check(expected is not None, "unexpected page error")
        joined += expected["text"]
        pages += 1
        if expected["complete"]:
            break
        check(expected["next_offset"] > offset, "next offset must advance")
        offset = expected["next_offset"]
    check(joined == data[block["start"]:block["end"]],
          "multipage bytes mismatch for %s#%s" % (name, identity))
    return pages


def refs_expected(files, name):
    _, refs = scan(files[name])
    shown = refs[:20]
    rows = []
    for reference in shown:
        rows.append((reference["name"], reference["source_id"],
                     reference["line"], reference["start"], reference["end"],
                     reference["name"], reference["id"],
                     target(files, reference["name"], reference["id"])))
    return len(refs), len(shown), rows


def check_refs(executable, path, files, name, digest):
    total, shown, rows = refs_expected(files, name)
    lines = probe(executable, path, "refs", name)
    check(lines[0] == "REFS %d %d" % (total, shown), "refs header %r" % lines[0])
    check(len(lines) == 1 + shown + 1, "refs line count %r" % lines)
    for line, row in zip(lines[1:], rows):
        fields = line.split()
        check(fields[0] == "REF", "ref row %r" % line)
        check(fields[1] == name, "ref source %r" % line)
        check(fields[2] == ("NONE" if row[1] is None else row[1]),
              "ref source id %r" % line)
        check([int(value) for value in fields[3:6]] == list(row[2:5]),
              "ref position %r" % line)
        check(fields[6:9] == list(row[5:8]), "ref target %r" % line)
    check(lines[-1] == "SOURCE " + digest, "refs source digest %r" % lines[-1])


def backlinks_expected(files, name, identity):
    state = target(files, name, identity)
    matches = []
    skipped = 0
    for source in sorted(files):
        try:
            _, refs = scan(files[source])
        except BlockError as error:
            if error.token == "INVALID_TEXT":
                skipped += 1
                continue
            raise
        for reference in refs:
            if (reference["name"], reference["id"]) == (name, identity):
                matches.append((source, reference["source_id"],
                                reference["line"], reference["start"],
                                reference["end"]))
    return len(matches), min(20, len(matches)), skipped, state, matches[:20]


def check_backlinks(executable, path, files, name, identity, digest):
    total, shown, skipped, state, rows = backlinks_expected(files, name, identity)
    lines = probe(executable, path, "backlinks", name, identity)
    check(lines[0] == "BACKLINKS %d %d %d %s" % (total, shown, skipped, state),
          "backlinks header %r" % lines[0])
    check(len(lines) == 1 + shown + 1, "backlinks line count %r" % lines)
    for line, row in zip(lines[1:], rows):
        fields = line.split()
        check(fields[0] == "BACKLINK", "backlink row %r" % line)
        check(fields[1] == row[0], "backlink source %r" % line)
        check(fields[2] == ("NONE" if row[1] is None else row[1]),
              "backlink source id %r" % line)
        check([int(value) for value in fields[3:6]] == list(row[2:5]),
              "backlink position %r" % line)
    check(lines[-1] == "SOURCE " + digest,
          "backlinks source digest %r" % lines[-1])


def many_blocks(count):
    return b"".join(b"@block b%02d\nx\n" % i for i in range(count))


def main():
    if len(sys.argv) != 2:
        print("usage: block_page_oracle.py BLOCK_QUERY_PROBE", file=sys.stderr)
        return 2
    executable = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="block-query-oracle-") as directory:
        basic = [
            ("irq.txt", note("IRQ", "@block entry\nLine one\nLine two\n"
                                    "@block tail\nend\n")),
            ("refs.txt", b"@block src\n[[irq.txt#entry]] and [[gone#x]]\n"),
            ("manyrefs", b"@block src\n" + b"[[irq.txt#entry]] " * 25),
            ("binary", b"@block a\n\x00\xff"),
        ]
        basic_path = write_image(directory, "basic.img", basic)
        basic_digest = source_digest(basic)
        for name in ("irq.txt", "refs.txt", "manyrefs"):
            check_list(executable, basic_path, dict(basic), name, basic_digest)
        expect_error(probe(executable, basic_path, "list", "missing"), "NOT_FOUND")

        irq_blocks = blocks_for(dict(basic), "irq.txt")
        for block in irq_blocks:
            check_read(executable, basic_path, dict(basic), "irq.txt",
                       block["id"], 0, basic_digest)
        check_read(executable, basic_path, dict(basic), "irq.txt", "entry", 2,
                   basic_digest)
        check_read(executable, basic_path, dict(basic), "irq.txt", "entry",
                   irq_blocks[0]["end"] - irq_blocks[0]["start"],
                   basic_digest)
        expect_error(probe(executable, basic_path, "read", "irq.txt", "gone", "0"),
                     "NOT_FOUND")
        check_refs(executable, basic_path, dict(basic), "refs.txt", basic_digest)
        check_refs(executable, basic_path, dict(basic), "manyrefs", basic_digest)
        check_backlinks(executable, basic_path, dict(basic), "gone", "x",
                        basic_digest)
        check_backlinks(executable, basic_path, dict(basic), "irq.txt", "entry",
                        basic_digest)
        expect_error(probe(executable, basic_path, "list", "refs.txt",
                           "00" * 32), "STALE")

        error_files = [
            ("good", b"@block a\nx"),
            ("bad", b"@yan-note 9\n@end\n"),
            ("binary", b"@block a\n\x00"),
        ]
        error_path = write_image(directory, "error.img", error_files)
        error_digest = source_digest(error_files)
        expect_error(probe(executable, error_path, "list", "bad"), "METADATA")
        expect_error(probe(executable, error_path, "list", "binary"),
                     "INVALID_TEXT")
        check_list(executable, error_path, dict(error_files), "good",
                   error_digest)

        many = [
            ("many64", many_blocks(64)),
            ("many65", many_blocks(65)),
            ("dup", b"@block same\nx\n@block same\n"),
        ]
        many_path = write_image(directory, "many.img", many)
        many_digest = source_digest(many)
        check_list(executable, many_path, dict(many), "many64", many_digest)
        expect_error(probe(executable, many_path, "list", "many65"), "LIMIT")
        expect_error(probe(executable, many_path, "list", "dup"), "AMBIGUOUS")

        pages = [
            ("p65535", b"@block a\n" + b"A" * 65535),
            ("p65536", b"@block a\n" + b"A" * 65536),
            ("p65537", b"@block a\n" + b"A" * 65537),
            ("u2", b"@block a\n" + b"a" * 65535 + b"\xc3\xa9tail"),
            ("u3", b"@block a\n" + b"a" * 65534 + b"\xe4\xb8\xadtail"),
            ("u4", b"@block a\n" + b"a" * 65533 + b"\xf0\x9f\x98\x80tail"),
            ("crlf", b"@block a\n" + b"a" * 65535 + b"\r\nb"),
            ("long", b"@block a\n" + b"x" * 100000),
            ("empty", b"@block a\n@block b\nx"),
        ]
        page_path = write_image(directory, "pages.img", pages)
        page_digest = source_digest(pages)
        page_files = dict(pages)
        for name in ("p65535", "p65536", "p65537", "long"):
            check_pages(executable, page_path, page_files, name, "a", page_digest)
        for name in ("u2", "u3", "u4", "crlf"):
            check_pages(executable, page_path, page_files, name, "a", page_digest)
        check_read(executable, page_path, page_files, "p65535", "a", 65535,
                   page_digest)
        check_read(executable, page_path, page_files, "p65537", "a", 65536,
                   page_digest)
        check_read(executable, page_path, page_files, "p65537", "a", 65537,
                   page_digest)
        check_read(executable, page_path, page_files, "p65537", "a", 65538,
                   page_digest)
        check_read(executable, page_path, page_files, "u3", "a", 65535,
                   page_digest)
        check_read(executable, page_path, page_files, "crlf", "a", 65536,
                   page_digest)
        check_read(executable, page_path, page_files, "empty", "a", 0,
                   page_digest)
        expect_error(probe(executable, page_path, "read", "p65537", "a", "999999"),
                     "INVALID")

    print("block_page_oracle: %d checks passed" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
