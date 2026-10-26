"""Pure controls for the 0036-R8 historical parity harness (issue #89).

These controls run on Windows with no Git history, no cross compiler, no
model and no MCP dependency. They exercise the SAME decoders, fixture
oracles and comparators the Linux operator entry uses, and each negative
receipt control asserts a specific ParityFailure code and field; process
completion controls require a specific harness failure.
"""

import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "guest"))

import yanfs_r8_parity as parity  # noqa: E402
import run_yanfs_r8_parity as entry  # noqa: E402


SERVER_META = {"io.modelcontextprotocol/serverInfo":
               {"name": "YanOS", "version": "0035"}}


def mcp_v1_wire(total=62, names=None, target="OK"):
    expected = parity.expected_mcp(63)
    names = list(expected["backlinks"] if names is None else names)
    structured = {
        "schema_version": 1,
        "operation": "block_backlinks",
        "source_sha256": "a" * 64,
        "name": "f0000.txt",
        "block_id": "x",
        "total": total,
        "shown": 20,
        "skipped": 0,
        "target_state": target,
        "backlinks": [
            {"source": name, "block_id": "x", "line": 6,
             "start": 49, "end": 64} for name in names
        ],
    }
    text = json.dumps(structured, separators=(",", ":"))
    result = {"isError": False, "structuredContent": structured,
              "content": [{"type": "text", "text": text}],
              "resultType": "complete", "_meta": SERVER_META}
    wire = {"jsonrpc": "2.0", "id": 1, "result": result}
    raw = json.dumps(wire, separators=(",", ":")).encode("utf-8")
    return raw, json.loads(raw)


def mcp_v2_wire():
    result = {"isError": True,
              "content": [{"type": "text", "text": '{"code":"CORRUPT"}'}],
              "resultType": "complete", "_meta": SERVER_META}
    wire = {"jsonrpc": "2.0", "id": 1, "result": result}
    raw = json.dumps(wire, separators=(",", ":")).encode("utf-8")
    return raw, json.loads(raw)


def terminal(files, case, image):
    expected = parity.expected_terminal(files, case)
    if case == "mount":
        capture = b"yanfs terminal\r\n" + parity.TRAILER
    elif case == "create":
        name = expected["plain_name"].encode("ascii")
        capture = (b"yanfs terminal\r\n" + b"yanfs> rm " + name +
                   b"\r\nOK rm\r\n" + b"yanfs> create " + name + b" " +
                   parity.PLAIN + b"\r\nOK create\r\n" + parity.TRAILER)
    else:
        command = {"ls": b"ls", "meta": b"meta list",
                   "backlinks": b"backlinks f0000.txt",
                   "related": b"related f0000.txt",
                   "block-backlinks": b"block backlinks f0000.txt#x",
                   "search": b"search x"}[case]
        capture = (b"yanfs terminal\r\n" + b"yanfs> " + command + b"\r\n" +
                   expected["body"] + expected["receipt"] + parity.TRAILER)
    status = {"verdict": "pass", "_tohost": 1, "out_length": len(capture),
              "delivered": len(capture)}
    return parity.TerminalObservation(capture, 0, status, image, image)


class _FakeStdin:
    def close(self):
        self.closed = True


class _FakeProcess:
    def __init__(self):
        self.stdin = _FakeStdin()
        self.returncode = 0

    def poll(self):
        return 0

    def communicate(self, timeout=None):
        return b"", b"server note\n"


def fake_mcp_module(raw):
    class Wire:
        def __init__(self, args, env):
            self.p = _FakeProcess()
            self.buffer = b""
            self.args = args

        def send(self, *_args, **_kwargs):
            return None

        def read_line(self, timeout):
            return raw

    def tool_value(reply, code=None):
        if isinstance(reply, list):
            raise TypeError("list indices must be integers or slices, not str")
        if "id" not in reply:
            raise KeyError("'id'")
        if "result" not in reply:
            raise KeyError("'result'")
        content = reply["result"].get("content")
        if not content:
            raise IndexError("list index out of range")
        json.loads(content[0]["text"])

    module = types.ModuleType("mcp_checks")
    module.Wire = Wire
    module.tool_value = tool_value
    return module


class HarnessControls(unittest.TestCase):

    def test_mcp_completion_diagnostics_and_exit(self):
        entry.check_mcp_completion(0, b"", b"ordinary diagnostic\n")
        controls = (
            (0, b"", b"ERROR: AddressSanitizer: control\n", "sanitizer"),
            (0, b"", b"runtime error: control\n", "sanitizer"),
            (-11, b"", b"", "server exit -11"),
            (5, b"", b"", "server exit 5"),
            (0, b"{}\n", b"", "trailing output"),
        )
        for code, extra, stderr, message in controls:
            with self.subTest(message=message):
                with self.assertRaisesRegex(entry.runtime.HarnessError, message):
                    entry.check_mcp_completion(code, extra, stderr)

    def test_mcp_schema_boundary_through_run_side(self):
        image = parity.encode(63)
        replies = (
            b"{}",
            b"[]",
            b'{"id":1}',
            b'{"jsonrpc":"2.0","id":1,"result":{"content":[]}}',
            b'{"jsonrpc":"2.0","id":1,"result":{"content":[{"text":"{"}]}}',
        )
        source = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix="yanos-r8-mcp-") as tmp:
            work = Path(tmp) / "work"
            work.mkdir()
            image_path = work / "filled-63.img"
            image_path.write_bytes(image)
            for index, raw in enumerate(replies):
                side = "boundary-%d" % index
                with self.subTest(raw=raw):
                    module = fake_mcp_module(raw)
                    with patch.dict(sys.modules, {"mcp_checks": module}):
                        with self.assertRaises(parity.ParityFailure) as caught:
                            entry.run_mcp_side(
                                work / "yan_mcp", image_path, image, 63, side,
                                parity.expected_mcp(63), work, source)
                    self.assertEqual(caught.exception.code, "MCP_SCHEMA")
                    self.assertEqual(caught.exception.field, "reply")
                    case_dir = work / "cases" / ("mcp-63-" + side)
                    self.assertEqual(
                        (case_dir / "response.line").read_bytes(), raw + b"\n")
                    self.assertIn(b"MCP_SCHEMA",
                                  (case_dir / "error.txt").read_bytes())
                    self.assertIn(b"server note",
                                  (case_dir / "stderr.bin").read_bytes())
                    self.assertEqual((case_dir / "exit.txt").read_bytes(),
                                     b"0\n")
                    self.assertEqual(
                        (case_dir / "partial.buffer").read_bytes(), b"")
                    argv = json.loads((case_dir / "server.argv.json")
                                      .read_text(encoding="utf-8"))
                    self.assertIsInstance(argv, list)
                    self.assertTrue(argv)

    def expect(self, code, field, action):
        with self.assertRaises(parity.ParityFailure) as caught:
            action()
        self.assertEqual(caught.exception.code, code)
        self.assertEqual(caught.exception.field, field)

    # ---------------------------------------------------------- positive

    def test_positive_terminal_cases(self):
        image = parity.encode(63)
        parity.validate_fixture(63, image)
        for case in parity.NORMAL_CASES:
            with self.subTest(case=case):
                observation = terminal(63, case, image)
                expected = parity.expected_terminal(63, case)
                parity.compare_terminal(observation, observation, expected, image)

    def search_observation(self, body):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "search")
        base = terminal(63, "search", image)
        capture = base.capture.replace(expected["body"], body)
        status = dict(base.status, out_length=len(capture),
                      delivered=len(capture))
        return parity.TerminalObservation(capture, 0, status, image, image), \
            expected, image

    def test_search_exact_20_rows_independent(self):
        names = parity.slot_names(63)[:10]
        body = b"".join(
            ("%s:5:@block x\r\n%s:6:[[f0000.txt#x]]\r\n" %
             (name, name)).encode("ascii") for name in names)
        expected = parity.expected_terminal(63, "search")
        self.assertEqual(body, expected["body"])
        observation, expected, image = self.search_observation(body)
        parity.compare_terminal(observation, observation, expected, image)

    def test_search_equal_both_wrong_rows(self):
        expected = parity.expected_terminal(63, "search")
        lines = expected["body"].splitlines(keepends=True)
        self.assertEqual(len(lines), 20)
        variants = {
            "19 rows": b"".join(lines[:-1]),
            "21 rows": b"".join(lines + [b"f9999.txt:5:@block x\r\n"]),
            "wrong name": b"".join([b"f9999.txt:5:@block x\r\n"] + lines[1:]),
            "wrong order": b"".join(lines[2:4] + lines[0:2] + lines[4:]),
        }
        for label, body in variants.items():
            with self.subTest(label=label):
                observation, expected, image = self.search_observation(body)
                self.expect("SEARCH_RESULTS", "search",
                            lambda: parity.compare_terminal(
                                observation, observation, expected, image))

    def test_positive_mcp_v1_and_v2(self):
        raw, reply = mcp_v1_wire()
        left = parity.McpObservation(raw, reply, 0, parity.encode(63),
                                     parity.encode(63))
        parity.compare_mcp(left, left, parity.expected_mcp(63), parity.encode(63))
        image = parity.encode(1023)
        raw2, reply2 = mcp_v2_wire()
        right = parity.McpObservation(raw2, reply2, 0, image, image)
        parity.compare_mcp(right, right, parity.expected_mcp(1023), image)

    def test_positive_corrupt_and_wrong_capture(self):
        image = parity.corrupt_duplicate(parity.encode(63))
        status = {"verdict": "guest", "_tohost": parity.CORRUPT_TOHOST,
                  "out_length": len(parity.CORRUPT_CAPTURE),
                  "delivered": len(parity.CORRUPT_CAPTURE)}
        good = parity.TerminalObservation(parity.CORRUPT_CAPTURE, 6, status,
                                          image, image)
        parity.compare_corrupt(good, good, image)
        bad = parity.TerminalObservation(b"ERROR IO\r\n", 6,
                                         dict(status, out_length=10,
                                              delivered=10),
                                         image, image)
        self.expect("CORRUPT_CAPTURE", "capture",
                    lambda: parity.check_corrupt_observation(bad, image))

    def test_delivered_mismatch(self):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "ls")
        good = terminal(63, "ls", image)
        bad = parity.TerminalObservation(
            good.capture, 0,
            dict(good.status, delivered=good.status["delivered"] - 1),
            image, image)
        self.expect("DELIVERED_MISMATCH", "delivered",
                    lambda: parity.check_terminal_observation(bad, expected, image))
        corrupt_image = parity.corrupt_duplicate(parity.encode(63))
        corrupt = parity.TerminalObservation(
            parity.CORRUPT_CAPTURE, 6,
            {"verdict": "guest", "_tohost": parity.CORRUPT_TOHOST,
             "out_length": len(parity.CORRUPT_CAPTURE),
             "delivered": len(parity.CORRUPT_CAPTURE) - 1},
            corrupt_image, corrupt_image)
        self.expect("CORRUPT_DELIVERED", "delivered",
                    lambda: parity.check_corrupt_observation(
                        corrupt, corrupt_image))

    # ----------------------------------------------------------- negative

    def test_one_byte_wrong_capture(self):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "ls")
        left = terminal(63, "ls", image)
        right = terminal(63, "ls", image)
        wrong = right.capture[:10] + b"X" + right.capture[11:]
        right = parity.TerminalObservation(wrong, 0, right.status, image, image)
        self.expect("CAPTURE_MISMATCH", "capture",
                    lambda: parity.compare_terminal(left, right, expected, image))

    def test_equal_empty_capture(self):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "ls")
        empty = parity.TerminalObservation(
            b"", 0, {"verdict": "pass", "_tohost": 1, "out_length": 0,
                     "delivered": 0},
            image, image)
        self.expect("EMPTY_CAPTURE", "capture",
                    lambda: parity.compare_terminal(empty, empty, expected, image))

    def test_missing_receipt(self):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "ls")
        observation = terminal(63, "ls", image)
        capture = observation.capture.replace(b"OK ls\r\n", b"")
        bad = parity.TerminalObservation(
            capture, 0, dict(observation.status, out_length=len(capture),
                             delivered=len(capture)),
            image, image)
        self.expect("MISSING_RECEIPT", "capture",
                    lambda: parity.compare_terminal(bad, bad, expected, image))

    def test_wrong_exit_verdict_tohost(self):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "ls")
        good = terminal(63, "ls", image)
        bad_exit = parity.TerminalObservation(good.capture, 6, good.status,
                                             image, image)
        self.expect("EXIT_INVALID", "exit",
                    lambda: parity.check_terminal_observation(
                        bad_exit, expected, image))
        bad_verdict = parity.TerminalObservation(
            good.capture, 0, dict(good.status, verdict="guest"), image, image)
        self.expect("STATUS_INVALID", "verdict",
                    lambda: parity.check_terminal_observation(
                        bad_verdict, expected, image))
        bad_tohost = parity.TerminalObservation(
            good.capture, 0, dict(good.status, _tohost=0x71000006), image, image)
        self.expect("TOHOST_INVALID", "tohost",
                    lambda: parity.check_terminal_observation(
                        bad_tohost, expected, image))

    def test_changed_image(self):
        image = parity.encode(63)
        expected = parity.expected_terminal(63, "ls")
        observation = terminal(63, "ls", image)
        changed = parity.TerminalObservation(
            observation.capture, 0, observation.status, image, image + b"x")
        self.expect("IMAGE_CHANGED", "image_sha256",
                    lambda: parity.compare_terminal(
                        changed, changed, expected, image))

    def test_mcp_one_byte_wrong(self):
        raw, reply = mcp_v1_wire()
        image = parity.encode(63)
        wrong = raw[:-1] + b"X"
        left = parity.McpObservation(raw, reply, 0, image, image)
        right = parity.McpObservation(wrong, reply, 0, image, image)
        self.expect("MCP_WIRE_MISMATCH", "wire",
                    lambda: parity.compare_mcp(left, right,
                                               parity.expected_mcp(63), image))

    def test_mcp_whitespace_and_key_order(self):
        raw, reply = mcp_v1_wire()
        image = parity.encode(63)
        expected = parity.expected_mcp(63)
        spaced = json.dumps(reply, separators=(", ", ": ")).encode("utf-8")
        self.assertEqual(json.loads(spaced), reply)
        left = parity.McpObservation(raw, reply, 0, image, image)
        right = parity.McpObservation(spaced, json.loads(spaced), 0,
                                      image, image)
        self.expect("MCP_WIRE_MISMATCH", "wire",
                    lambda: parity.compare_mcp(left, right, expected, image))
        reordered = {"result": reply["result"], "id": reply["id"],
                     "jsonrpc": reply["jsonrpc"]}
        raw_order = json.dumps(reordered, separators=(",", ":")).encode("utf-8")
        self.assertEqual(json.loads(raw_order), reply)
        right = parity.McpObservation(raw_order, json.loads(raw_order), 0,
                                      image, image)
        self.expect("MCP_WIRE_MISMATCH", "wire",
                    lambda: parity.compare_mcp(left, right, expected, image))

    def test_mcp_equal_empty(self):
        image = parity.encode(63)
        empty = parity.McpObservation(b"", None, 0, image, image)
        self.expect("MCP_EMPTY", "raw",
                    lambda: parity.compare_mcp(empty, empty,
                                               parity.expected_mcp(63), image))

    def test_mcp_malformed(self):
        image = parity.encode(63)
        bad = parity.McpObservation(b"{", None, 0, image, image)
        self.expect("MCP_JSON", "raw",
                    lambda: parity.compare_mcp(bad, bad,
                                               parity.expected_mcp(63), image))

    def test_mcp_process_abnormal(self):
        image = parity.encode(63)
        raw, reply = mcp_v1_wire()
        bad = parity.McpObservation(raw, reply, 5, image, image)
        self.expect("MCP_EXIT", "exit",
                    lambda: parity.compare_mcp(bad, bad,
                                               parity.expected_mcp(63), image))

    def test_mcp_v1_wrong_equal_fields(self):
        image = parity.encode(63)
        expected = parity.expected_mcp(63)
        raw, reply = mcp_v1_wire(total=61)
        left = parity.McpObservation(raw, reply, 0, image, image)
        self.expect("MCP_FIELD", "total",
                    lambda: parity.compare_mcp(left, left, expected, image))
        names = list(expected["backlinks"])
        names[0] = "f9999.txt"
        raw, reply = mcp_v1_wire(names=names)
        left = parity.McpObservation(raw, reply, 0, image, image)
        self.expect("MCP_BACKLINK_NAME", "backlinks.source",
                    lambda: parity.compare_mcp(left, left, expected, image))

    def test_mcp_v2_wrong_equal_success(self):
        image = parity.encode(1023)
        raw, reply = mcp_v1_wire()
        left = parity.McpObservation(raw, reply, 0, image, image)
        self.expect("MCP_NOT_ERROR", "isError",
                    lambda: parity.compare_mcp(left, left,
                                               parity.expected_mcp(1023), image))

    # ------------------------------------------------------------- matrix

    def test_matrix_exact(self):
        matrix = parity.assert_case_matrix(parity.case_matrix())
        self.assertEqual(len(matrix), 33)

    def test_matrix_zero_missing_extra_duplicate(self):
        full = list(parity.case_matrix())
        self.expect("MATRIX_LENGTH", "matrix",
                    lambda: parity.assert_case_matrix([]))
        self.expect("MATRIX_MISSING", "matrix",
                    lambda: parity.assert_case_matrix(full[1:]))
        self.expect("MATRIX_EXTRA", "matrix",
                    lambda: parity.assert_case_matrix(full + [(63, "bogus")]))
        self.expect("MATRIX_DUPLICATE", "matrix",
                    lambda: parity.assert_case_matrix(full + [full[0]]))
    # -------------------------------------------------------- named inputs

    def test_named_inputs_ignore_foreign_directory(self):
        with tempfile.TemporaryDirectory(prefix="yanos-r8-") as tmp:
            root = Path(tmp)
            (root / "needed.txt").write_text("needed", encoding="utf-8")
            foreign = root / "foreign"
            foreign.mkdir()
            (foreign / "changing.tmp").write_text("outside", encoding="utf-8")
            original = os.scandir

            def fail(*_args, **_kwargs):
                raise OSError("the harness scanned an unrelated directory")

            os.scandir = fail
            try:
                parity.require_named_inputs(root, ["needed.txt"])
                self.expect("MISSING_INPUT", "inputs",
                            lambda: parity.require_named_inputs(
                                root, ["missing.txt"]))
            finally:
                os.scandir = original

    def test_named_input_path_escape(self):
        with tempfile.TemporaryDirectory(prefix="yanos-r8-") as tmp:
            self.expect("INPUT_PATH", "inputs",
                        lambda: parity.require_named_inputs(Path(tmp), ["../x"]))

    def test_work_gate_refuses_populated_without_writes(self):
        with tempfile.TemporaryDirectory(prefix="yanos-r8-work-") as tmp:
            root = Path(tmp)
            work = root / "work"
            work.mkdir()
            sentinel = work / "manifest.json"
            sentinel.write_bytes(b"sentinel")
            before = {path.name: path.read_bytes() for path in work.iterdir()}
            self.expect("WORK_NOT_FRESH", "work",
                        lambda: parity.check_fresh_work(work, root / "source"))
            self.assertEqual(sentinel.read_bytes(), b"sentinel")
            self.assertEqual({path.name: path.read_bytes()
                              for path in work.iterdir()}, before)
            self.expect("WORK_NOT_FRESH", "work",
                        lambda: parity.check_fresh_work(work, work))
            self.assertEqual(sentinel.read_bytes(), b"sentinel")
            self.assertEqual({path.name: path.read_bytes()
                              for path in work.iterdir()}, before)
            empty = root / "empty"
            empty.mkdir()
            self.assertEqual(parity.check_fresh_work(empty, root / "source"),
                             empty.resolve())
            self.assertEqual(list(empty.iterdir()), [])

    # ------------------------------------------------------- fixture oracle

    def test_fixture_invariants_and_permutation(self):
        for files in parity.SIZES:
            with self.subTest(files=files):
                image = parity.encode(files)
                info = parity.validate_fixture(files, image)
                self.assertTrue(info["contiguous"])
                self.assertTrue(info["name_order_permuted"])
                names = parity.slot_names(files)
                self.assertEqual(names[0], "f0000.txt")
                self.assertEqual(names[1], "f%04d.txt" % (files - 1))
                self.assertNotEqual(names, sorted(names))

    def test_corrupt_duplicate_and_overlap(self):
        image = parity.encode(63)
        duplicate = parity.corrupt_duplicate(image)
        self.expect("DUPLICATE_NAME", "name",
                    lambda: parity.check_unique_names(
                        parity.decode(duplicate)["entries"]))
        overlap = parity.corrupt_overlap(image)
        info = parity.decode(overlap)
        self.expect("EXTENT_OVERLAP", "extents",
                    lambda: parity.check_extents(
                        info["entries"], info["directory_blocks"],
                        info["capacity_blocks"]))

    def test_gap_and_crc(self):
        image = parity.encode(63)
        info = parity.decode(image)
        last = info["entries"][-1]
        mutated = bytearray(image)
        block, offset = parity.entry_location(62, 1)
        parity._put_u32(mutated, block * parity.BLOCK + offset + 36,
                        last["start"] + 1)
        gap = parity.recompute_crc(mutated)
        gap_info = parity.decode(gap)
        self.expect("EXTENT_NOT_CONTIGUOUS", "extents",
                    lambda: parity.check_extents(
                        gap_info["entries"], gap_info["directory_blocks"],
                        gap_info["capacity_blocks"]))
        bad_crc = bytearray(image)
        bad_crc[parity.HEADER + 32] ^= 1
        self.expect("CRC_MISMATCH", "crc",
                    lambda: parity.decode(bytes(bad_crc)))

    def test_historical_inventory_excludes_only_later_internal_headers(self):
        later={"os/block_peek.h","os/note_metadata_format_internal.h"}
        self.assertTrue(later.issubset(entry.runtime.GUEST_HEADERS))
        for label in ('baseline','candidate'):
            required=set(entry.required_ref_inputs(label))
            self.assertFalse(later & required)
            self.assertEqual(label=='candidate','os/sort.h' in required)
            self.assertTrue(set(entry.runtime.GUEST_HEADERS)-later-{'os/sort.h'} <= required)

    def test_no_private_paths_in_sources(self):
        here = Path(__file__).resolve().parent
        forbidden = ("/mnt" + "/e", "E:" + "/YanOS")
        for name in ("yanfs_r8_parity.py", "test_yanfs_r8_parity.py",
                     "guest/run_yanfs_r8_parity.py"):
            text = (here / name).read_text(encoding="utf-8")
            for marker in forbidden:
                self.assertNotIn(marker, text, name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
