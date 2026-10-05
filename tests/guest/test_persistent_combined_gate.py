"""Negative controls for tests/guest/run_persistent_combined.py's verdict rules.

The combined persistent case is a gate: it decides whether a planted defect was
detected. These controls protect that decision, and every one of them feeds the
gate a run that must *not* be counted as a detection:

  * the two controls the review reproduced against the first version of the
    gate: a reader that exits 6 after only a runtime panic (no `pcombined:
    FAIL` line at all), and a reader whose marker is its own spawn failure
    (code 0x50000003 phase 0), which says nothing about the mutant under test;
  * a Guest assertion with the right code but the wrong phase, and a marker
    whose code the executor does not report (or reports differently);
  * a reader panic, a signal death, a sanitizer report and a timeout;
  * a host check that is not the one the mutant declared (and the deliberate
    absence of any "the host caught something, so accept it" fallback).

They also re-run the gate's existing controls: the classifier self-test, the
host byte comparison's two named failure modes, the no-effect mutant that must
survive and the hanging-wait mutant that must stay INCONCLUSIVE.

Run directly (`python3 tests/guest/test_persistent_combined_gate.py`) or through
the case gate, which runs this file before it builds anything. It is registered
with CTest as `persistent_combined_gate`, so the default suite runs it too; it
mocks both Guest processes and therefore needs no cross compiler.
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parent / "run_persistent_combined.py"
# Loading the gate must not leave a __pycache__ behind in the source tree.
sys.dont_write_bytecode = True


def load_gate():
    spec = importlib.util.spec_from_file_location("run_persistent_combined_gate", MODULE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gate = load_gate()

# The writer's console, exactly as the real case prints it: enough evidence for
# the host's own checks to pass, so each control below isolates one reader (or
# writer) verdict instead of tripping over the stage before it.
WRITER_OK = "\r\n".join((
    "pcombined: capacity blocks=8",
    "pcombined: fast write lba=3 bytes=4096 polls=1 peer-ticks=0",
    "pcombined: backpressure used=8184 free=7 reply=16",
    "pcombined: window round=0 kind=write budget=64 ticks=64 polls=1 frames=171",
    "pcombined: window round=0 awaited=write lba=1 status=ok count=1",
    "pcombined: window round=1 kind=read budget=32 ticks=32 polls=1 frames=1",
    "pcombined: window round=1 read-back lba=1 peer-read=pattern-a match=1",
    "pcombined: PASS",
)) + "\r\n"

READER_OK = "\r\n".join((
    "pcombined: reader capacity blocks=8",
    "pcombined: reader lba=1 bytes=4096 match=1 pattern=pattern-a",
    "pcombined: reader lba=3 bytes=4096 match=1 pattern=pattern-c",
    "pcombined: reader lba=2 bytes=4096 match=1 pattern=seed",
    "pcombined: reader PASS",
)) + "\r\n"


def executor_stderr(code):
    """The line yan_run prints to stderr before it exits 6."""
    return f"yan_run: the Guest reported failure code {code} after 123 instructions\n"


def run_isolated(writer, reader, on_reader=None):
    """Runs the real run_combined with both Guest processes mocked. `writer` and
    `reader` are (returncode, stdout, stderr) triples; `on_reader(image)` runs
    between the two processes, so a control can change the file the way a
    misbehaving second process would. The image starts as the host's own
    expectation, so the writer stage and the host byte comparison pass."""
    with tempfile.TemporaryDirectory() as directory:
        image = Path(directory) / "disk.img"
        image.write_bytes(gate.expected_image())
        calls = []

        def fake_run(command, timeout=None):
            calls.append(command)
            if len(calls) == 1:
                return subprocess.CompletedProcess(command, *writer)
            if on_reader is not None:
                on_reader(image)
            return subprocess.CompletedProcess(command, *reader)

        with patch.object(gate, "run_process", side_effect=fake_run):
            return gate.run_combined("executor", "writer", "reader", image)


def detected_outcome(source, code=None, phase=None):
    """A synthetic outcome carrying one named detection, for the tests that
    compare expectations without spawning anything."""
    return {"kind": "DETECTED", "detection": {"verdict": "DETECTED", "source": source,
                                              "code": code, "phase": phase},
            "code": code, "phase": phase, "problem": source, "host_check": source,
            "reader_kind": "not-run"}


class ReproducedReviewControls(unittest.TestCase):
    """The two controls the review reproduced against the first version of the
    gate, calling mutation_verdict exactly as the review harness does."""

    def test_reader_runtime_panic_is_not_a_detection(self):
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "", "yan_run: the Guest reported failure code 2147483650"))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])
        self.assertNotEqual(
            gate.mutation_verdict("host", "ASSERTION", gate.FAIL_DRAIN_PAYLOAD, 20, outcome),
            "DETECTED", outcome)
        self.assertNotEqual(gate.verdict_for(gate.WRONG_BLOCK_EXPECT, outcome), "DETECTED")

    def test_reader_spawn_failure_marker_is_not_a_detection(self):
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000003 phase=0\n", ""))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])
        self.assertNotEqual(
            gate.mutation_verdict("host", "ASSERTION", gate.FAIL_DRAIN_PAYLOAD, 20, outcome),
            "DETECTED", outcome)

    def test_reader_spawn_failure_with_executor_line_is_not_a_detection(self):
        # The realistic shape of the same failure: marker and executor agree,
        # so the reader assertion is real - it is simply not what wrong-block
        # declared, and declaring a host check must not accept it.
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000003 phase=0\n", executor_stderr(0x50000003)))
        self.assertEqual(outcome["kind"], "DETECTED", outcome)
        self.assertEqual(outcome["detection"]["source"], gate.READER_ASSERTION)
        verdict = gate.verdict_for(gate.WRONG_BLOCK_EXPECT, outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)


class GuestAssertionEvidence(unittest.TestCase):
    """rc6 is only an assertion when the Guest's marker and the executor's own
    report agree; everything else is a harness error."""

    def test_wrong_phase_is_not_a_detection(self):
        outcome = run_isolated(
            (6, "pcombined: FAIL code=0x50000014 phase=20\n", executor_stderr(0x50000014)),
            (0, "", ""))
        self.assertEqual(outcome["detection"],
                         gate.guest_detection(gate.WRITER_ASSERTION, 0x50000014, 20))
        verdict = gate.verdict_for(gate.FAKE_WAIT_EXPECT, outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)
        self.assertIn("phase", verdict)

    def test_wrong_code_is_not_a_detection(self):
        outcome = run_isolated(
            (6, "pcombined: FAIL code=0x50000013 phase=10\n", executor_stderr(0x50000013)),
            (0, "", ""))
        verdict = gate.verdict_for(gate.FAKE_WAIT_EXPECT, outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)

    def test_marker_and_executor_code_must_agree(self):
        outcome = run_isolated(
            (6, "pcombined: FAIL code=0x50000014 phase=10\n", executor_stderr(0x50000013)),
            (0, "", ""))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])
        self.assertIn("disagree", outcome["problem"])

    def test_missing_marker_is_not_a_detection(self):
        outcome = run_isolated((6, "", executor_stderr(0x80000002)), (0, "", ""))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])
        self.assertIn("without a pcombined FAIL marker", outcome["problem"])

    def test_marker_without_executor_report_is_not_a_detection(self):
        outcome = run_isolated(
            (6, "pcombined: FAIL code=0x50000014 phase=10\n", ""), (0, "", ""))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])

    def test_reader_wrong_phase_is_not_a_detection(self):
        # The first version never parsed the reader's code or phase at all, so
        # this is the path that let any reader assertion through.
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000011 phase=21\n", executor_stderr(0x50000011)))
        verdict = gate.verdict_for(
            gate.guest_detection(gate.READER_ASSERTION, gate.FAIL_DRAIN_PAYLOAD, 20), outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)
        self.assertIn("phase", verdict)

    def test_reader_wrong_code_is_not_a_detection(self):
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000003 phase=0\n", executor_stderr(0x50000003)))
        verdict = gate.verdict_for(
            gate.guest_detection(gate.READER_ASSERTION, gate.FAIL_DRAIN_PAYLOAD, 20), outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)

    def test_reader_marker_and_executor_must_agree(self):
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000011 phase=20\n", executor_stderr(0x50000003)))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])

    def test_legacy_assertion_expectation_is_writer_only(self):
        # Right code and phase, but the reader asserted it: the compatibility
        # entry point must not accept it for a declared writer assertion.
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000011 phase=20\n", executor_stderr(0x50000011)))
        self.assertNotEqual(
            gate.mutation_verdict("host", "ASSERTION", gate.FAIL_DRAIN_PAYLOAD, 20, outcome),
            "DETECTED", outcome)


class DetectionSources(unittest.TestCase):
    """Each mutant names one check; no source stands in for another."""

    def test_reader_assertion_cannot_satisfy_a_host_check_expectation(self):
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000011 phase=20\n", executor_stderr(0x50000011)))
        verdict = gate.verdict_for(gate.WRONG_BLOCK_EXPECT, outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)
        self.assertIn(gate.HOST_IMAGE_BYTES, verdict)

    def test_host_check_cannot_satisfy_a_guest_assertion_expectation(self):
        verdict = gate.verdict_for(gate.OMIT_WRITE_EXPECT,
                                   detected_outcome(gate.HOST_IMAGE_BYTES))
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)
        self.assertIn(gate.WRITER_ASSERTION, verdict)

    def test_host_checks_are_named_not_interchangeable(self):
        self.assertEqual(
            gate.verdict_for(gate.WRONG_BLOCK_EXPECT,
                             detected_outcome(gate.HOST_IMAGE_BYTES)),
            "DETECTED")
        for source in (gate.HOST_IMAGE_LENGTH, gate.READER_CHANGED_IMAGE,
                       gate.WRITER_ASSERTION):
            verdict = gate.verdict_for(gate.WRONG_BLOCK_EXPECT, detected_outcome(source))
            self.assertTrue(verdict.startswith("HARNESS-ERROR"), (source, verdict))

    def test_reader_that_changes_the_image_is_named(self):
        def scribble(image):
            data = bytearray(image.read_bytes())
            data[5] ^= 0x01
            image.write_bytes(data)

        outcome = run_isolated((0, WRITER_OK, ""), (0, READER_OK, ""), on_reader=scribble)
        self.assertEqual(outcome["kind"], "DETECTED", outcome)
        self.assertEqual(outcome["detection"]["source"], gate.READER_CHANGED_IMAGE)
        verdict = gate.verdict_for(gate.WRONG_BLOCK_EXPECT, outcome)
        self.assertTrue(verdict.startswith("HARNESS-ERROR"), verdict)


class HarnessErrors(unittest.TestCase):
    """Crashes, sanitizer reports, timeouts and missing evidence stay harness
    errors and can never be counted as detections."""

    def test_reader_crash_is_not_a_detection(self):
        outcome = run_isolated((0, WRITER_OK, ""), (-11, "", ""))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])

    def test_reader_sanitizer_report_is_not_a_detection(self):
        outcome = run_isolated(
            (0, WRITER_OK, ""),
            (6, "pcombined: FAIL code=0x50000011 phase=20\n",
             executor_stderr(0x50000011) + "ERROR: AddressSanitizer: heap-buffer-overflow\n"))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])

    def test_reader_timeout_is_not_a_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "disk.img"
            image.write_bytes(gate.expected_image())
            calls = []

            def fake_run(command, timeout=None):
                calls.append(command)
                if len(calls) == 1:
                    return subprocess.CompletedProcess(command, 0, WRITER_OK, "")
                raise RuntimeError(f"process timed out: {command}")

            with patch.object(gate, "run_process", side_effect=fake_run):
                with self.assertRaises(RuntimeError):
                    gate.run_combined("executor", "writer", "reader", image)

    def test_reader_without_evidence_is_a_harness_error(self):
        outcome = run_isolated((0, WRITER_OK, ""), (0, "pcombined: reader PASS\r\n", ""))
        self.assertEqual(outcome["kind"], "HARNESS-ERROR", outcome)
        self.assertIsNone(outcome["detection"])


class ExistingControls(unittest.TestCase):
    """The controls the gate already had keep their meaning."""

    def test_no_effect_mutant_survives(self):
        outcome = run_isolated((0, WRITER_OK, ""), (0, READER_OK, ""))
        self.assertEqual(outcome["kind"], "PASS", outcome)
        self.assertEqual(gate.verdict_for(gate.NO_EFFECT_EXPECT, outcome), "SURVIVED")

    def test_survived_control_rejects_a_detection(self):
        verdict = gate.verdict_for(gate.NO_EFFECT_EXPECT,
                                   detected_outcome(gate.WRITER_ASSERTION, 0x50000014, 10))
        self.assertNotEqual(verdict, "SURVIVED")

    def test_inconclusive_control_rejects_a_detection(self):
        verdict = gate.verdict_for(gate.PURE_HANG_EXPECT,
                                   detected_outcome(gate.WRITER_ASSERTION, 0x50000014, 10))
        self.assertNotEqual(verdict, "INCONCLUSIVE")

    def test_classifier_keeps_crashes_and_sanitizers_out(self):
        gate.classify_self_test()

    def test_host_byte_comparison_names_its_two_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            gate.check_the_check(Path(directory))


if __name__ == "__main__":
    unittest.main(verbosity=2)
