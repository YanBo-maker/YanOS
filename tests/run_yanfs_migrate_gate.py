#!/usr/bin/env python3
"""Permanent F3 gate: the acceptance harness must never erase the caller work.

Checks, in order:
1. a call with three nonexistent required tools exits 1 (not 77) and leaves
   unrelated marker files in --work untouched;
2. a normal call creates a fresh run-* child, keeps the markers and does not
   scan or delete unrelated files.
3. a missing explicitly supplied CLI fault binary fails before creating a run;
   an external dependency skip never hides an assertion failure.

The gate itself only reads the marker files; it does not enumerate unrelated
directories as a cleanup source.
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=str(cwd), text=True, capture_output=True)


def exit_priority(harness: Path, work: Path) -> None:
    """An external dependency skip must never hide an assertion failure."""
    spec = importlib.util.spec_from_file_location("migration_gate_subject", harness)
    subject = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = subject
    try:
        spec.loader.exec_module(subject)

        def failure(h, *args):
            h.record("control.assertion", "gate", "FAIL", "injected assertion")

        def dependency(h, *args):
            h.record("control.external-compiler", "gate", "SKIP", "compiler absent")
            h.exit_code = 77

        replacements = {name: lambda *args: None for name in (
            "add_ac1_cases", "add_ac2_cases", "add_cli_boundary_cases",
            "add_fault_cases", "add_robustness_cases", "add_cli_fault_cases",
            "add_term_cases", "add_oracle_self_checks", "add_detection_case",
            "add_asset_detection_case", "add_semantic_case")}
        replacements.update(prepare_fixtures=lambda work: {},
                            validate_explicit_inputs=lambda args: True,
                            add_detection_case=dependency)
        with tempfile.TemporaryDirectory(prefix="exit-priority-", dir=work) as owned:
            for has_failure, expected in ((True, 1), (False, 77)):
                replacements["add_ac1_cases"] = failure if has_failure else lambda *args: None
                with patch.multiple(subject, **replacements), contextlib.redirect_stdout(io.StringIO()):
                    actual = subject.main([
                        "--migrate", "unused", "--terms-probe", "unused",
                        "--fault", "unused", "--work", owned])
                if actual != expected:
                    raise AssertionError(f"failure={has_failure}: exit {actual} != {expected}")
    finally:
        del sys.modules[spec.name]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="0036-R7 work-directory gate")
    parser.add_argument("--migrate", required=True)
    parser.add_argument("--terms-probe", required=True)
    parser.add_argument("--fault", required=True)
    parser.add_argument("--work", default=None)
    parser.add_argument("--harness", default=None)
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parent.parent
    harness = Path(args.harness).resolve() if args.harness else (
        root / "tests" / "run_yanfs_migrate.py")
    if not harness.is_file():
        print(f"GATE FAIL: harness missing: {harness}", file=sys.stderr)
        return 1
    work = Path(args.work).resolve() if args.work else Path(tempfile.mkdtemp(
        prefix="yanfs_migrate_gate_", dir=str(root / "build" / "20261021-r7" / "b")))
    work.mkdir(parents=True, exist_ok=True)
    first = work / "unrelated-marker.txt"
    second = work / "unrelated-second.txt"
    first.write_text("keep me\n")
    second.write_text("keep me too\n")

    missing = work / "does-not-exist"
    bad = run([sys.executable, str(harness),
               "--migrate", str(missing / "migrate"),
               "--terms-probe", str(missing / "terms"),
               "--fault", str(missing / "fault"),
               "--work", str(work)], work)
    if bad.returncode != 1:
        print(f"GATE FAIL: missing-tool exit {bad.returncode} != 1", file=sys.stderr)
        print(bad.stderr, file=sys.stderr)
        return 1
    if not first.is_file() or first.read_text() != "keep me\n":
        print("GATE FAIL: first marker removed by missing-tool call", file=sys.stderr)
        return 1
    if not second.is_file() or second.read_text() != "keep me too\n":
        print("GATE FAIL: second marker removed by missing-tool call", file=sys.stderr)
        return 1

    explicit_missing = run([
        sys.executable, str(harness),
        "--migrate", str(Path(args.migrate).resolve()),
        "--terms-probe", str(Path(args.terms_probe).resolve()),
        "--fault", str(Path(args.fault).resolve()),
        "--cli-fault", str(missing / "cli-fault"),
        "--work", str(work)], work)
    if explicit_missing.returncode != 1 or "WORK " in explicit_missing.stdout:
        print("GATE FAIL: missing explicit CLI fault input did not fail before setup",
              file=sys.stderr)
        return 1
    if first.read_text() != "keep me\n" or second.read_text() != "keep me too\n":
        print("GATE FAIL: explicit missing input changed markers", file=sys.stderr)
        return 1

    good = run([sys.executable, str(harness),
                "--migrate", str(Path(args.migrate).resolve()),
                "--terms-probe", str(Path(args.terms_probe).resolve()),
                "--fault", str(Path(args.fault).resolve()),
                "--work", str(work)], work)
    if good.returncode != 0:
        print(f"GATE FAIL: normal call exit {good.returncode} != 0", file=sys.stderr)
        print(good.stdout, file=sys.stderr)
        print(good.stderr, file=sys.stderr)
        return 1
    if not first.is_file() or first.read_text() != "keep me\n":
        print("GATE FAIL: first marker removed by normal call", file=sys.stderr)
        return 1
    if not second.is_file() or second.read_text() != "keep me too\n":
        print("GATE FAIL: second marker removed by normal call", file=sys.stderr)
        return 1
    children = [entry for entry in work.iterdir() if entry.is_dir() and
                entry.name.startswith("run-")]
    if not children:
        print("GATE FAIL: normal call did not create a run-* child", file=sys.stderr)
        return 1
    exit_priority(harness, work)
    print(f"GATE PASS work={work} run_children={len(children)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
