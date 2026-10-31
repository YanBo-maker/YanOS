#!/usr/bin/env python3
"""0041-R2-AC3, Guest half: the MCP listing and the Guest terminal must agree.

The AC says a single-tag `yanos_note_list` result is item-for-item identical to
`meta list tag:<same>` on the Guest terminal. That is a cross-implementation
claim, so it cannot be asserted from the MCP side alone: the same image is read
once through the MCP tool and once through the production Guest ELF, and the
two name lists are compared.

The MCP half of AC3 (a single tag returns only files carrying it) lives in
``tests/mcp_note_list_checks.py``; both files carry the ``0041-R2-AC3`` label so
``ctest -L 0041-R2-AC3`` locates both halves.

This is registered only when the Guest targets exist, so a platform without a
Guest toolchain does not run it at all rather than silently skipping.
"""

import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
TESTS = HERE.parents[1]
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(HERE.parent))

from mcp_checks import META, Wire  # noqa: E402
from semantic_fixture import image  # noqa: E402
import run_search_terms  # noqa: E402
import run_terminal_files as base  # noqa: E402

LIST = "yanos_note_list"
CASES = 0


def note(tags=(), title="T", body="B\n"):
    head = "@yan-note 1\ntitle: %s\n" % title
    for tag in tags:
        head += "tag: %s\n" % tag
    return (head + "@end\n@block log\n" + body).encode("utf-8")


# Deliberately mixes files that share tags, carry only one, and carry none, so
# a single-tag filter has something to exclude. Names are ASCII so the Guest
# ordering and the MCP ordering are directly comparable.
LIBRARY = [
    ("a-cpu.txt", note(["cpu", "riscv"])),
    ("b-cpu.txt", note(["cpu"])),
    ("c-os.txt", note(["os"])),
    ("d-both.txt", note(["cpu", "os"])),
    ("e-old.txt", b"header-less old note\n"),
]


class Server:
    def __init__(self, binary, image_path):
        self.image = Path(image_path)
        self.ident = 0
        self.wire = Wire([str(binary), "--disk-image", str(self.image),
                          "--index", str(self.image) + ".index",
                          "--model", str(self.image) + ".model",
                          "--runner", str(self.image) + ".runner",
                          "--allow-meta"], dict(os.environ))

    def call(self, name, arguments):
        self.ident += 1
        self.wire.send("tools/call", ident=self.ident,
                       params={"name": name, "arguments": arguments})
        reply = self.wire.get()
        result = reply["result"]
        value = json.loads(result["content"][0]["text"])
        if result["isError"]:
            return None, value
        return value, None

    def close(self):
        self.wire.close()


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def guest_names(args, disk, work, tag, label):
    """Run `meta list tag:<tag>` on the production Guest and return its names.

    The Guest prints one name per line then `OK meta list shown=N`; the reply
    is matched exactly, so a different set OR a different order both fail.
    """
    command = ("meta list tag:" + tag).encode()
    result = base.run_guest(args.run, args.guest, disk,
                            command + b"\nexit\n", work, label)
    base.check_not_harness(result, label)
    base.require_rc(result, 0, label)
    lines = []
    started = False
    for raw in result.stdout.split(b"\r\n"):
        if raw.startswith(b"OK meta list shown="):
            break
        if raw.startswith(b"yanfs> "):
            # The prompt line echoes the command; the names follow it.
            started = True
            continue
        if not started:
            # Skip the terminal banner that precedes the first prompt.
            continue
        if raw:
            lines.append(raw.decode("ascii"))
    return lines, result


def mcp_names(binary, disk, tag):
    server = Server(binary, disk)
    try:
        value, error = server.call(LIST, {"tags": [tag]})
        assert error is None, ("mcp_list_error", tag, error)
        return value["names"]
    finally:
        server.close()


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="0041-R2-AC3 Guest agreement for yanos_note_list")
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    args = parser.parse_args()

    for path in (args.server, args.run, args.guest):
        if not path.is_file():
            print("FAIL fixture missing: %s" % path, file=sys.stderr)
            return 1

    global CASES
    with tempfile.TemporaryDirectory(prefix="yan-note-list-agree-") as tmp:
        root = Path(tmp)
        disk = root / "agree.img"
        disk.write_bytes(image(LIBRARY))
        before = digest(disk)

        for tag in ("cpu", "os", "riscv", "nosuchtag"):
            from_mcp = mcp_names(args.server, disk, tag)
            from_guest, result = guest_names(args, disk, root, tag,
                                             "guest-" + tag)
            assert from_mcp == from_guest, (
                "AC3 disagreement for tag %r\n  MCP   %r\n  Guest %r\n  raw %r"
                % (tag, from_mcp, from_guest, result.stdout))
            # The Guest receipt also pins the count; compare it to shown.
            expected_receipt = ("OK meta list shown=%d\r\n" % len(from_mcp)).encode()
            assert expected_receipt in result.stdout, (
                "guest receipt mismatch", tag, result.stdout)
            CASES += 1

        # The Guest ran read-only, so the image must be untouched.
        assert digest(disk) == before, "guest_agreement_wrote_image"

    print("AC3 Guest agreement: %d tag(s) matched" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
