#!/usr/bin/env python3
"""Issue 78/79 harness controls for the budget boundary decision.

These are synthetic ELF32 spans only. They prove that `boundary_failures`
consumes the measured arena value supplied by the caller, that the exact
32 MiB boundary passes while one byte over fails, and that truncated or
structurally invalid headers do not succeed. The real Guest measurement and
the nine-command detection proof remain separate.

The CLI priority controls use a temporary tree containing only the declared
arena inputs and stub Guest ELF/mkfs files. Those stubs are path-validation
only and are never executed; the controls stop at the local-input or compiler
availability guard.
"""
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

BUDGET = Path(__file__).resolve().parent / "guest" / "run_arena_budget.py"
REPO = Path(__file__).resolve().parents[1]
ACCOUNTING = REPO / "tests" / "guest" / "run_arena_accounting.py"
SPEC = importlib.util.spec_from_file_location("arena_budget_boundary", BUDGET)
budget = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(budget)

LIMIT = 32 * 1024 * 1024
FORMULA_4095 = 1003358


def copy_declared_inputs(root, skip=None):
    """Copy only the explicit arena declared inputs, never a workspace tree."""
    source = root / "source"
    for relative in budget.arena_measure.ARENA_INPUTS:
        if relative == skip:
            continue
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((REPO / relative).read_bytes())
    return source


def stub_file(path):
    """Path-validation stub only; it is never executed as a Guest or mkfs."""
    path.write_bytes(b"path-validation-only\n")
    return path


def invoke(script, source, work, guest, mkfs, cc):
    return subprocess.run(
        [sys.executable, "-B", str(script), "--source", str(source),
         "--work", str(work), "--guest", str(guest), "--mkfs", str(mkfs),
         "--cc", str(cc)],
        cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)



def synthetic_elf(load_bytes):
    """A minimal ELF32 with one PT_LOAD span of `load_bytes`."""
    data = bytearray(84)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<I", data, 28, 52)        # e_phoff
    struct.pack_into("<HH", data, 42, 32, 1)    # e_phentsize, e_phnum
    struct.pack_into("<I", data, 52, 1)         # PT_LOAD
    struct.pack_into("<I", data, 60, 0x80000000)  # p_vaddr
    struct.pack_into("<I", data, 72, load_bytes)  # p_memsz
    return bytes(data)


class BudgetBoundaryControls(unittest.TestCase):
    def test_measured_size_at_limit_passes_and_one_more_fails(self):
        measured = 1_000_000
        elf = synthetic_elf(LIMIT - measured)
        self.assertEqual([], budget.boundary_failures(elf, measured))
        over = budget.boundary_failures(elf, measured + 1)
        self.assertTrue(over)
        self.assertTrue(any("exceed" in failure for failure in over), over)

    def test_decision_uses_the_provided_measurement_not_the_formula(self):
        elf = synthetic_elf(1024)
        # The formula-sized 4095 arena fits this tiny load span.
        self.assertEqual([], budget.boundary_failures(elf, FORMULA_4095))
        # The caller's measured value is larger than the whole 32 MiB budget.
        over = budget.boundary_failures(elf, LIMIT + 1)
        self.assertTrue(over)
        self.assertTrue(any("exceed" in failure for failure in over), over)

    def test_truncated_and_invalid_headers_do_not_succeed(self):
        invalid_phentsize = bytearray(synthetic_elf(0))
        struct.pack_into("<H", invalid_phentsize, 42, 40)
        cases = {
            "empty": b"",
            "short magic": b"\x7fELF\x01\x01\x01",
            "missing program header": synthetic_elf(0)[:52],
            "bad phentsize": bytes(invalid_phentsize),
        }
        for label, data in cases.items():
            with self.subTest(label=label):
                failures = budget.boundary_failures(data, FORMULA_4095)
                self.assertTrue(failures, label)
                self.assertTrue(any("cannot decode" in failure
                                    for failure in failures), failures)


    def test_actual_cli_rejects_missing_declared_production_with_available_cc(self):
        with tempfile.TemporaryDirectory(prefix="arena-cli-input-") as temp:
            root = Path(temp)
            source = copy_declared_inputs(root, skip="os/yanfs.c")
            guest = stub_file(root / "guest.elf")
            mkfs = stub_file(root / "mkfs")
            # sys.executable is an existing executable, not a C compiler. The
            # local guard must reject before any compile, so availability alone
            # cannot mask the missing declared production source.
            for script in (ACCOUNTING, BUDGET):
                with self.subTest(script=script.name):
                    work = root / ("work-" + script.stem)
                    result = invoke(script, source, work, guest, mkfs,
                                    sys.executable)
                    output = result.stdout + result.stderr
                    self.assertNotIn(result.returncode, (0, 77), output)
                    self.assertIn("missing required local input", output)
                    self.assertIn(str(Path("os") / "yanfs.c"), output)
                    self.assertFalse(work.exists())

    def test_actual_cli_local_absence_beats_missing_cc(self):
        with tempfile.TemporaryDirectory(prefix="arena-cli-local-") as temp:
            root = Path(temp)
            healthy = copy_declared_inputs(root)
            guest = stub_file(root / "guest.elf")
            mkfs = stub_file(root / "mkfs")
            missing_cc = root / "missing-cc"
            cases = {
                "source": (root / "missing-source", guest, mkfs),
                "guest": (healthy, root / "missing-guest", mkfs),
                "mkfs": (healthy, guest, root / "missing-mkfs"),
            }
            for script in (ACCOUNTING, BUDGET):
                for label, (source_arg, guest_arg, mkfs_arg) in cases.items():
                    with self.subTest(script=script.name, missing=label):
                        work = root / ("work-%s-%s"
                                       % (script.stem, label))
                        result = invoke(script, source_arg, work, guest_arg,
                                        mkfs_arg, missing_cc)
                        output = result.stdout + result.stderr
                        self.assertNotIn(result.returncode, (0, 77), output)
                        self.assertIn("missing required local input", output)
                        self.assertFalse(work.exists())

    def test_actual_cli_healthy_local_with_only_missing_cc_is_77(self):
        with tempfile.TemporaryDirectory(prefix="arena-cli-skip-") as temp:
            root = Path(temp)
            source = copy_declared_inputs(root)
            guest = stub_file(root / "guest.elf")
            mkfs = stub_file(root / "mkfs")
            missing_cc = root / "missing-cc"
            for script in (ACCOUNTING, BUDGET):
                with self.subTest(script=script.name):
                    work = root / ("work-" + script.stem)
                    result = invoke(script, source, work, guest, mkfs,
                                    missing_cc)
                    output = result.stdout + result.stderr
                    self.assertEqual(77, result.returncode, output)
                    self.assertIn("SKIP host compiler", result.stdout)
                    self.assertFalse(work.exists())


if __name__ == "__main__":
    unittest.main()
