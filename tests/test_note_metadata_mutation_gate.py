"""Pure classification controls; these are not actual mutation detections."""
import unittest
from pathlib import Path
import tempfile
import subprocess
import sys
from run_note_metadata_mutation import classify_suites, REQUIRED
from run_yanfs_mutation import unity_suite


class MetadataGate(unittest.TestCase):
    def run_case(self, records, rc=None, err=b""):
        failures = sum(row[1] == "FAIL" for row in records)
        return failures if rc is None else rc, unity_suite(records), err

    def test_exact_suite_owner_assertion(self):
        run = self.run_case([("owner", "FAIL", "exact")])
        self.assertEqual("owner-fail", classify_suites({"format": run}, "owner", "exact"))

    def test_wrong_owner(self):
        run = self.run_case([("other", "FAIL", "exact")])
        self.assertEqual("wrong-owner", classify_suites({"format": run}, "owner", "exact"))

    def test_wrong_assertion(self):
        run = self.run_case([("owner", "FAIL", "other")])
        self.assertEqual("wrong-assertion", classify_suites({"format": run}, "owner", "exact"))

    def test_wrong_suite(self):
        good = self.run_case([("owner", "PASS")])
        bad = self.run_case([("owner", "FAIL", "exact")])
        self.assertEqual("wrong-suite", classify_suites({"format": good, "service": bad}, "owner", "exact"))

    def test_mixed_harness_wins(self):
        bad = self.run_case([("owner", "FAIL", "exact")])
        self.assertEqual("harness", classify_suites({"format": bad, "service": (1, b"", b"")}, "owner", "exact"))

    def test_signal_timeout_sanitizer_ignored_and_footer_mismatch(self):
        raw = unity_suite([("owner", "FAIL", "exact")])
        for run in [(None, b"", b""), (-11, raw, b""), (1, raw, b"runtime error:"),
                    (0, raw, b""), (1, b"FAIL", b""),
                    self.run_case([("owner", "FAIL", "exact"), ("other", "IGNORE")])]:
            with self.subTest(run=run):
                self.assertEqual("harness", classify_suites({"format": run}, "owner", "exact"))

    def test_green_no_effect_is_not_detection(self):
        run = self.run_case([("owner", "PASS")])
        self.assertEqual("pass", classify_suites({"format": run}, "owner", "exact"))

    def test_required_inputs_hard_fail_before_external_skip(self):
        self.assertIn("os/sort.h", REQUIRED)
        script = Path(__file__).with_name("run_note_metadata_mutation.py")
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for name in REQUIRED:
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("placeholder")
            def invoke():
                return subprocess.run([sys.executable, "-B", str(script), "--repo", str(root),
                    "--unity", str(root / "absent-unity"), "--cc", "absent-compiler-yanos",
                    "--work", str(root / "work")], capture_output=True, timeout=20)
            self.assertEqual(77, invoke().returncode)
            for name in REQUIRED:
                with self.subTest(missing=name):
                    p = root / name
                    saved = p.read_bytes()
                    p.unlink()
                    result = invoke()
                    p.write_bytes(saved)
                    self.assertEqual(1, result.returncode)
                    self.assertIn(b"missing repository inputs", result.stdout)


if __name__ == "__main__":
    unittest.main()
