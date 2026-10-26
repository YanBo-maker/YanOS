#!/usr/bin/env python3
"""Issue #83 focused matrix for the asset EOF probe.

Default mode asserts the fixed behavior only:

* a healthy migration through the production CLI still succeeds;
* one EINTR and two consecutive EINTR at the source or target post-copy EOF
  probe are retried, for an empty and a non-empty attachment;
* an extra tail byte or a real EIO at the same probe still produces the
  historical VERIFY diagnostic and removes the newly created target;
* a control proves an injection that misses the probe (wrong asset name) has
  no marker and is rejected by the same marker assertion.

``--expect-old-red`` is baseline evidence only, for the pre-fix binary: every
EINTR case must hit the exact probe, exit 9/VERIFY, print the exact existing
diagnostic, clean NEW and NEW.assets, and leave OLD image/assets unchanged.
RED is printed only after all of those checks.  The RED mode is not registered
in CMake.

The fault control lives in tests/yanfs_migrate_fault.c.  It gates on the
actual OLD.assets/NEW.assets directory tree, the exact asset basename,
count == 1, current offset == fstat(st_size), and post-copy verification; it
does not guess from a global read counter.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path


class CheckError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckError(message)


def load_acceptance():
    path = Path(__file__).resolve().parent / "run_yanfs_migrate.py"
    spec = importlib.util.spec_from_file_location(
        "yanfs_migrate_eintr_subject", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse_markers(stderr: str) -> list[dict[str, str]]:
    markers: list[dict[str, str]] = []
    for line in stderr.splitlines():
        if not line.startswith("FAULT kind="):
            continue
        fields: dict[str, str] = {}
        for token in line.split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        markers.append(fields)
    return markers


class EintrRunner:
    def __init__(self, args: argparse.Namespace, module) -> None:
        self.args = args
        self.m = module
        self.migrate = Path(args.migrate).resolve()
        self.fault = Path(args.fault).resolve()
        self.work = Path(args.work).resolve()
        self.evidence = (Path(args.evidence).resolve()
                         if args.evidence else None)
        self.results: list[dict] = []
        self.failures = 0
        self.passes = 0

    # ---- infrastructure ----
    def run(self, argv: list[str], env: dict | None = None,
            timeout: int = 30) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(argv, cwd=str(self.work), env=env,
                                  text=True, capture_output=True,
                                  timeout=timeout)
        except FileNotFoundError as error:
            raise CheckError(f"cannot execute {argv[0]}: {error}") from error
        except subprocess.TimeoutExpired as error:
            raise CheckError(f"timeout: {' '.join(argv)}") from error

    def record(self, name: str, status: str, detail: str,
               run: dict | None = None) -> None:
        entry = {"name": name, "status": status, "detail": detail}
        if run is not None:
            entry["run"] = run
        self.results.append(entry)
        if status == "PASS" or status == "RED":
            self.passes += 1
        else:
            self.failures += 1
        suffix = f" {detail}" if detail else ""
        print(f"[{status}] {name}{suffix}", flush=True)

    def case(self, name: str, function, evidence: bool = True) -> None:
        try:
            run = function()
        except CheckError as error:
            self.record(name, "FAIL", str(error))
        except Exception as error:  # noqa: BLE001 - report, never hide
            self.record(name, "FAIL", f"{type(error).__name__}: {error}")
            if self.args.trace:
                traceback.print_exc()
        else:
            detail = run.pop("detail", "") if evidence and run else ""
            self.record(name, "PASS", detail, run if evidence else None)

    def red_case(self, name: str, function) -> None:
        try:
            run = function()
        except CheckError as error:
            self.record(name, "FAIL", str(error))
        except Exception as error:  # noqa: BLE001 - report, never hide
            self.record(name, "FAIL", f"{type(error).__name__}: {error}")
            if self.args.trace:
                traceback.print_exc()
        else:
            detail = run.pop("detail", "") if run else ""
            self.record(name, "RED", detail, run)

    # ---- source identity ----
    def source_identity(self, old: Path) -> dict:
        return {
            "image_sha256": self.m.sha256_file(old),
            "assets_sha256": self.m.asset_hash(self.m.assets_path(old)),
        }

    def binary_identity(self) -> dict:
        return {
            "migrate": str(self.migrate),
            "migrate_sha256": self.m.sha256_file(self.migrate),
            "fault": str(self.fault),
            "fault_sha256": self.m.sha256_file(self.fault),
        }

    def run_record(self, argv: list[str], env: dict | None,
                   proc: subprocess.CompletedProcess,
                   before: dict) -> dict:
        selected = {}
        if env is not None:
            for key in ("MIG_EOF_FAULT", "MIG_EOF_ASSET"):
                if key in env:
                    selected[key] = env[key]
        return {
            "argv": argv,
            "env": selected,
            "rc": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "before": before,
            "binaries": self.binary_identity(),
        }

    # ---- assertions ----
    def assert_probe_marker(self, stderr: str, kind: str, role: str,
                            asset: str, attempts: list[int]) -> list[dict]:
        markers = [entry for entry in parse_markers(stderr)
                   if entry.get("kind") == kind]
        require(bool(markers),
                f"injection marker missing for kind={kind} asset={asset}")
        require(len(markers) == len(attempts),
                f"injection marker count {len(markers)} != {len(attempts)} "
                f"for kind={kind}")
        selected: list[dict] = []
        for attempt in attempts:
            matches = [entry for entry in markers
                       if entry.get("attempt") == str(attempt)]
            require(bool(matches),
                    f"injection marker attempt={attempt} missing for "
                    f"kind={kind}")
            marker = matches[0]
            require(marker.get("role") == role,
                    f"marker role={marker.get('role')} != {role}")
            require(marker.get("asset") == asset,
                    f"marker asset={marker.get('asset')} != {asset}")
            require(marker.get("phase") == "verify",
                    f"marker phase={marker.get('phase')} != verify")
            require(marker.get("count") == "1",
                    f"marker count={marker.get('count')} != 1")
            require(marker.get("after_new") == "1",
                    "injection marker did not prove post-creation state")
            require(marker.get("offset") == marker.get("size"),
                    f"marker offset={marker.get('offset')} != "
                    f"size={marker.get('size')}")
            require(int(marker["offset"]) >= 0, "negative marker offset")
            selected.append(marker)
        return selected

    def verify_success(self, old: Path, new: Path, before: dict) -> None:
        require(new.is_file(), "success did not create NEW")
        old_image = self.m.decode_image(old)
        new_image = self.m.decode_image(new)
        directory_blocks = (1023 + 1) // 64
        expected_capacity = (old_image.capacity +
                             directory_blocks - old_image.directory_blocks)
        require(new_image.version == 2, f"NEW version {new_image.version} != 2")
        require(new_image.directory_blocks == directory_blocks,
                "NEW directory block count differs")
        require(new_image.entry_capacity == 1023,
                f"NEW entry capacity {new_image.entry_capacity} != 1023")
        require(new_image.capacity == expected_capacity,
                f"NEW capacity {new_image.capacity} != {expected_capacity}")
        old_by_slot = old_image.by_slot
        new_by_slot = new_image.by_slot
        require(set(old_by_slot) == set(new_by_slot),
                "live slot set differs")
        for slot in sorted(old_by_slot):
            left = old_by_slot[slot]
            right = new_by_slot[slot]
            require(left.name == right.name,
                    f"slot {slot} name differs: {left.name!r} vs {right.name!r}")
            require(left.size == right.size,
                    f"slot {slot} size differs")
            require(left.data == right.data, f"slot {slot} bytes differ")
        require(not self.m.check_compact(new_image), "NEW is not compact")
        require(not self.m.check_tail_padding(new_image),
                "NEW tail padding is not zero")
        require(self.m.source_sha256(old_image) == self.m.source_sha256(new_image),
                "source_sha256 differs")
        old_assets = self.m.asset_manifest(self.m.assets_path(old))
        require(self.m.asset_manifest(self.m.assets_path(new)) == old_assets,
                "NEW.assets differs from OLD.assets")
        require(self.m.sha256_file(old) == before["image_sha256"],
                "OLD image bytes changed")
        require(self.m.asset_hash(self.m.assets_path(old)) ==
                before["assets_sha256"], "OLD.assets changed")
        require(Path(os.path.realpath(str(old)) + ".lock").is_file(),
                "source .lock missing")

    def verify_failure(self, old: Path, new: Path, before: dict,
                       stderr: str) -> None:
        require("EVENT new_created" in stderr,
                "failure evidence lacks EVENT new_created")
        require(not os.path.lexists(new), "failed migration left NEW behind")
        require(not os.path.lexists(self.m.assets_path(new)),
                "failed migration left NEW.assets behind")
        require(self.m.sha256_file(old) == before["image_sha256"],
                "OLD image bytes changed")
        require(self.m.asset_hash(self.m.assets_path(old)) ==
                before["assets_sha256"], "OLD.assets changed")
        require(Path(os.path.realpath(str(old)) + ".lock").is_file(),
                "source .lock missing after failure")

    def fault_argv(self, old: Path, new: Path) -> list[str]:
        return [str(self.fault), "--from", str(old), "--to", str(new),
                "--files", "1023"]

    def healthy_argv(self, old: Path, new: Path) -> list[str]:
        return [str(self.migrate), "--from", str(old), "--to", str(new),
                "--files", "1023"]

    @staticmethod
    def fault_env(kind: str | None, asset: str | None = None) -> dict:
        env = dict(os.environ)
        if kind is not None:
            env["MIG_EOF_FAULT"] = kind
        if asset is not None:
            env["MIG_EOF_ASSET"] = asset
        return env


def prepare_fixture(module, run_dir: Path) -> Path:
    old = run_dir / "issue83-source.img"
    module.build_image(old, 24, 1, {
        0: ("alpha.txt", b"alpha shared line\n"),
        3: ("empty.txt", b""),
        6: ("blob.bin", bytes(((i * 11) + 5) & 0xFF for i in range(700))),
    })
    root = module.assets_path(old)
    (root / "img" / "deep").mkdir(parents=True)
    (root / "payload.bin").write_bytes(
        bytes(((i * 31) + 9) & 0xFF for i in range(20000)))
    (root / "img" / "deep" / "blob.bin").write_bytes(
        bytes(((i * 29) + 7) & 0xFF for i in range(517)))
    (root / "empty.bin").write_bytes(b"")
    (root / "empty").mkdir()
    return old


def eintr_matrix(runner: EintrRunner, old: Path, out: Path,
                 red: bool) -> None:
    for role in ("source", "target"):
        for asset in ("payload.bin", "empty.bin"):
            label = asset.replace("/", "-")
            for action, attempts in (("eintr1", [1]), ("eintr2", [1, 2])):
                kind = f"asset_eof_{role}_{action}"
                name = f"EINTR.{role}.{label}.{action}"
                new = out / f"{name.replace('.', '-')}.img"
                before = runner.source_identity(old)
                argv = runner.fault_argv(old, new)
                env = runner.fault_env(kind, asset)

                def one(before=before, argv=argv, env=env, new=new,
                        kind=kind, role=role, asset=asset, attempts=attempts,
                        action=action, name=name):
                    proc = runner.run(argv, env=env)
                    record = runner.run_record(argv, env, proc, before)
                    marker_attempts = [1] if red else attempts
                    runner.assert_probe_marker(proc.stderr, kind, role,
                                               asset, marker_attempts)
                    if red:
                        require(proc.returncode == 9,
                                f"old implementation exit "
                                f"{proc.returncode} != 9 (unexpected success)"
                                if proc.returncode == 0 else
                                f"old implementation exit "
                                f"{proc.returncode} != 9")
                        require("MIGFAULT result=8" in proc.stderr,
                                "old implementation did not report VERIFY")
                        require(
                            f"asset '{asset}' size changed during verification"
                            in proc.stderr,
                            "historical VERIFY diagnostic missing")
                        require(proc.stdout == "",
                                "old implementation printed stdout")
                        runner.verify_failure(old, new, before, proc.stderr)
                        record["detail"] = "old=VERIFY/9 cleaned"
                        return record
                    require(proc.returncode == 0,
                            f"EINTR {kind} exit {proc.returncode} != 0; "
                            f"stderr={proc.stderr.strip()[:400]}")
                    require("MIGFAULT result=0" in proc.stderr,
                            "retried probe did not complete the migration")
                    require(proc.stdout == "",
                            "fault driver printed unexpected stdout")
                    runner.verify_success(old, new, before)
                    record["detail"] = f"attempts={attempts}"
                    return record

                if red:
                    runner.red_case(name, one)
                else:
                    runner.case(name, one)


def negative_case(runner: EintrRunner, old: Path, out: Path, role: str,
                  asset: str, action: str, attempts: list[int],
                  marker: str) -> None:
    label = asset.replace("/", "-")
    kind = f"asset_eof_{role}_{action}"
    name = f"EOF-{action.upper()}.{role}.{label}"
    new = out / f"{name.replace('.', '-')}.img"
    before = runner.source_identity(old)
    argv = runner.fault_argv(old, new)
    env = runner.fault_env(kind, asset)

    def one():
        proc = runner.run(argv, env=env)
        record = runner.run_record(argv, env, proc, before)
        runner.assert_probe_marker(proc.stderr, kind, role, asset, attempts)
        require(proc.returncode == 9,
                f"{name}: exit {proc.returncode} != 9")
        require("MIGFAULT result=8" in proc.stderr,
                f"{name}: library result is not VERIFY")
        require(
            f"asset '{asset}' size changed during verification"
            in proc.stderr,
            f"{name}: exact historical diagnostic missing")
        require(proc.stdout == "", f"{name}: printed stdout")
        runner.verify_failure(old, new, before, proc.stderr)
        record["detail"] = f"{marker} -> VERIFY/9 cleaned"
        return record

    runner.case(name, one)


def negative_matrix(runner: EintrRunner, old: Path, out: Path) -> None:
    for role in ("source", "target"):
        for asset in ("payload.bin", "empty.bin"):
            for action, marker in (("tail", "extra tail byte"),
                                   ("eio", "real read error")):
                negative_case(runner, old, out, role, asset, action, [1],
                              marker)
        # The legal signal interruption is followed by the same negative
        # boundary on the retried probe: one non-empty asset is sufficient.
        for action, marker in (("eintr_tail", "EINTR then extra tail byte"),
                               ("eintr_eio", "EINTR then real read error")):
            negative_case(runner, old, out, role, "payload.bin", action,
                          [1, 2], marker)


def healthy_case(runner: EintrRunner, old: Path, out: Path) -> None:
    new = out / "healthy.img"
    before = runner.source_identity(old)
    argv = runner.healthy_argv(old, new)

    def one():
        proc = runner.run(argv)
        record = runner.run_record(argv, None, proc, before)
        require(proc.returncode == 0,
                f"healthy migration exit {proc.returncode}; "
                f"stderr={proc.stderr.strip()[:400]}")
        require(proc.stdout.startswith("yan_migrate: migrated "),
                f"success stdout missing or wrong: {proc.stdout!r}")
        require(proc.stderr == "",
                f"healthy migration wrote stderr: {proc.stderr!r}")
        runner.verify_success(old, new, before)
        return record

    runner.case("HEALTHY.no-injection", one)


def missing_marker_control(runner: EintrRunner, old: Path, out: Path) -> None:
    """A probe miss (wrong asset) must be rejected by the marker assertion."""
    new = out / "control-missing-marker.img"
    argv = runner.fault_argv(old, new)
    env = runner.fault_env("asset_eof_source_eintr1", "absent-asset.bin")

    def one():
        proc = runner.run(argv, env=env)
        require(proc.returncode == 0,
                f"control migration exit {proc.returncode} != 0")
        require("FAULT kind=asset_eof_source_eintr1" not in proc.stderr,
                "control unexpectedly hit the EOF probe")
        try:
            runner.assert_probe_marker(proc.stderr,
                                       "asset_eof_source_eintr1",
                                       "source", "absent-asset.bin", [1])
        except CheckError:
            return {"detail": "marker miss rejected"}
        raise CheckError("missing-marker control unexpectedly passed")

    runner.case("CONTROL.missing-marker-rejected", one)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Issue #83 asset EOF probe EINTR matrix")
    parser.add_argument("--migrate", required=True,
                        help="production yan_migrate CLI")
    parser.add_argument("--fault", required=True,
                        help="yanfs_migrate_fault driver with MIG_EOF_FAULT")
    parser.add_argument("--work", required=True,
                        help="private work directory for fixtures and runs")
    parser.add_argument("--expect-old-red", action="store_true",
                        help="baseline-only mode: require the old VERIFY/9 "
                             "behavior for every EINTR case")
    parser.add_argument("--evidence", default=None,
                        help="optional directory for cases.json evidence")
    parser.add_argument("--trace", action="store_true")
    return parser.parse_args(argv)


def validate(args: argparse.Namespace) -> bool:
    for label, path in (("migrate", args.migrate), ("fault", args.fault)):
        candidate = Path(path)
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            print(f"FATAL explicit input missing or unusable: {label}={path}",
                  file=sys.stderr)
            return False
    return True


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not validate(args):
        return 1
    module = load_acceptance()
    work = Path(args.work).resolve()
    if work.exists() and not work.is_dir():
        print(f"FATAL --work is not a directory: {work}", file=sys.stderr)
        return 1
    work.mkdir(parents=True, exist_ok=True)
    run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=str(work)))
    print(f"WORK {run_dir}")
    runner = EintrRunner(args, module)
    out = run_dir / "out"
    out.mkdir()
    old = prepare_fixture(module, run_dir)
    identities = {
        "mode": "old-red" if args.expect_old_red else "fixed",
        "argv": argv,
        "cwd": os.getcwd(),
        "source_image_sha256": module.sha256_file(old),
        "source_assets_sha256": module.asset_hash(module.assets_path(old)),
    }
    identities.update(runner.binary_identity())
    print(json.dumps(identities, sort_keys=True))

    if args.expect_old_red:
        eintr_matrix(runner, old, out, red=True)
    else:
        healthy_case(runner, old, out)
        eintr_matrix(runner, old, out, red=False)
        negative_matrix(runner, old, out)
        missing_marker_control(runner, old, out)

    print("\n=== Issue #83 asset EOF probe matrix ===")
    for entry in runner.results:
        print(f"{entry['status']:4} {entry['name']:46} {entry['detail']}")
    print(f"\nPASS={runner.passes} FAIL={runner.failures}")

    if args.evidence:
        evidence = Path(args.evidence).resolve()
        evidence.mkdir(parents=True, exist_ok=True)
        payload = dict(identities)
        payload["cases"] = runner.results
        (evidence / "cases.json").write_text(
            json.dumps(payload, indent=2, ensure_ascii=False) + "\n")
        print(f"EVIDENCE={evidence / 'cases.json'}")

    print(f"WORK={run_dir}")
    if runner.failures:
        return 1
    if args.expect_old_red:
        print("OLD_RED_COMPLETE")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
