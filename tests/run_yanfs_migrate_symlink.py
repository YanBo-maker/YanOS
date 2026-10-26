#!/usr/bin/env python3
"""Issue88 / 0036-R7: symlinked source image vs. alias-side `.assets` decoy.

The approved source attachment is ``realpath(OLD) + ".assets"``.  A directory
whose name spells ``<alias>.assets`` next to the link is a decoy and must not be
copied.  Three variants are covered: relative v1, absolute v2 with a high slot,
and a source with no canonical assets but a present decoy.

The real ``yan_migrate`` CLI is run as a subprocess.  Each case stops at its
first failed assertion and keeps the exact argv, rc, stdout and stderr.
"""
from __future__ import annotations

import argparse
import json
import os
import stat
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

try:
    from run_yanfs_migrate import (BLOCK, asset_manifest, build_image,
                                   check_compact, check_tail_padding,
                                   decode_image, make_asset_fixture,
                                   sha256_file, source_sha256)
except Exception as error:  # noqa: BLE001 - missing oracle is a hard stop
    print(f"FATAL cannot import the independent oracle: {error}",
          file=sys.stderr)
    raise SystemExit(1)

AC1 = "0036-R7-AC1"
SUCCESS_PREFIX = "yan_migrate: migrated "


def expect(case: dict, name: str, condition: bool, detail: str = "") -> None:
    if not condition:
        raise AssertionError(f"{name}: {detail}" if detail else name)
    case["checks"].append({"name": name, "status": "PASS"})


def alias_state(path: Path) -> tuple:
    info = os.lstat(path)
    return (info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode), info.st_size,
            info.st_mtime_ns, os.readlink(path), os.path.realpath(path))


def make_decoy(alias: Path) -> Path:
    """Decoy beside the link; never realpath(alias)+'.assets'."""
    decoy = Path(str(alias) + ".assets")
    (decoy / "img").mkdir(parents=True)
    (decoy / "img" / "x.png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + b"DECOY-ISSUE88" + bytes(range(32, 96)))
    (decoy / "decoy-only.bin").write_bytes(
        bytes(((index * 91) + 17) & 0xFF for index in range(2048)))
    (decoy / "decoy-only-dir").mkdir()
    (decoy / "decoy-only-dir" / "unique.dat").write_bytes(
        b"unique decoy payload, not part of the canonical tree\n")
    return decoy


def build_fixtures(run: Path) -> list[dict]:
    data, links = run / "data", run / "links"
    for directory in (data, links, run / "out"):
        directory.mkdir()
    cases: list[dict] = []

    v1 = data / "v1.img"
    v1_image = build_image(v1, 256, 1, {
        0: ("alpha.txt", b"alpha v1 line one\nalpha line two\n"),
        1: ("empty.txt", b""),
        3: ("blob.bin",
            bytes(((index * 13) + 7) & 0xFF for index in range(5000)) +
            b"\xa5\xfe\x00\x01"),
    })
    make_asset_fixture(v1)
    v1_alias = links / "v1-rel.img"
    os.symlink("../data/v1.img", v1_alias)
    cases.append({
        "name": "SYMLINK.rel-v1", "real": v1, "image": v1_image,
        "canonical": Path(os.path.realpath(str(v1)) + ".assets"),
        "alias": v1_alias, "decoy": make_decoy(v1_alias), "files": 1023,
        "to": "out/rel-new.img", "has_assets": True,
    })

    v2 = data / "v2high.img"
    v2_image = build_image(v2, 64, 16, {
        5: ("five.txt", b"slot five v2 high fixture\n"),
        1000: ("high.bin", b"high slot 1000 binary\n" + bytes(range(256))),
    })
    make_asset_fixture(v2)
    v2_alias = links / "v2high-abs.img"
    os.symlink(str(v2), v2_alias)
    cases.append({
        "name": "SYMLINK.abs-v2-high", "real": v2, "image": v2_image,
        "canonical": Path(os.path.realpath(str(v2)) + ".assets"),
        "alias": v2_alias, "decoy": make_decoy(v2_alias), "files": 2047,
        "to": "out/abs-v2-new.img", "has_assets": True,
    })

    plain = data / "plain.img"
    plain_image = build_image(plain, 128, 1, {
        0: ("plain.txt", b"plain source with no canonical assets\n"),
        2: ("empty.txt", b""),
    })
    plain_alias = links / "plain-rel.img"
    os.symlink("../data/plain.img", plain_alias)
    cases.append({
        "name": "SYMLINK.decoy-only", "real": plain, "image": plain_image,
        "canonical": Path(os.path.realpath(str(plain)) + ".assets"),
        "alias": plain_alias, "decoy": make_decoy(plain_alias), "files": 1023,
        "to": "out/plain-new.img", "has_assets": False,
    })
    return cases


def save_raw(run: Path, name: str, argv: list[str],
             proc: subprocess.CompletedProcess) -> None:
    raw = run / "raw" / name
    raw.mkdir(parents=True)
    (raw / "migrate.argv").write_text(" ".join(argv) + f"\ncwd={run}\n")
    (raw / "migrate.stdout").write_text(proc.stdout)
    (raw / "migrate.stderr").write_text(proc.stderr)
    (raw / "migrate.rc").write_text(f"{proc.returncode}\n")


def run_case(cfg: dict, run: Path, migrate: Path) -> dict:
    case = {
        "name": cfg["name"], "ac": AC1, "status": "FAIL",
        "failed_assertion": "", "checks": [], "commands": [],
        "fixture": {
            "real": str(cfg["real"]), "alias": str(cfg["alias"]),
            "decoy": str(cfg["decoy"]), "canonical": str(cfg["canonical"]),
            "canonical_present": cfg["has_assets"],
            "target": str(run / cfg["to"]),
            "target_assets": str(Path(str(run / cfg["to"]) + ".assets")),
        },
    }
    try:
        old = cfg["image"]
        image_sha = sha256_file(cfg["real"])
        canonical = asset_manifest(cfg["canonical"])
        decoy = asset_manifest(cfg["decoy"])
        alias = alias_state(cfg["alias"])
        expect(case, "alias.is-unresolved-symlink-to-real",
               os.path.islink(cfg["alias"])
               and str(cfg["alias"]) != os.path.realpath(str(cfg["alias"]))
               and os.path.realpath(str(cfg["alias"]))
               == os.path.realpath(str(cfg["real"])),
               f"alias={cfg['alias']} readlink={os.readlink(cfg['alias'])!r}")
        if cfg["has_assets"]:
            expect(case, "fixture.decoy-not-canonical",
                   decoy != canonical
                   and any(record[0].startswith("decoy-only")
                           for record in (decoy or [])),
                   f"canonical={canonical} decoy={decoy}")
        argv = [str(migrate), "--from", str(cfg["alias"]), "--to", cfg["to"],
                "--files", str(cfg["files"])]
        try:
            proc = subprocess.run(argv, cwd=str(run), text=True,
                                  capture_output=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            case["status"] = "ERROR"
            case["failed_assertion"] = f"migration.execute: {error}"
            return case
        save_raw(run, cfg["name"], argv, proc)
        case["commands"].append({
            "argv": argv, "cwd": str(run), "rc": proc.returncode,
            "stdout": proc.stdout, "stderr": proc.stderr,
        })
        expect(case, "migration.exit-0", proc.returncode == 0,
               f"rc={proc.returncode} stderr={proc.stderr.strip()[:400]}")
        expect(case, "migration.success-stdout",
               bool(proc.stdout.strip()) and proc.stdout.startswith(SUCCESS_PREFIX),
               f"stdout={proc.stdout!r}")
        expect(case, "migration.stderr-empty", proc.stderr == "",
               f"stderr={proc.stderr!r}")

        target = run / cfg["to"]
        try:
            new = decode_image(target)
        except Exception as error:  # noqa: BLE001
            raise AssertionError(
                f"image.new-decodes: {type(error).__name__}: {error}") from error
        directory_blocks = (cfg["files"] + 1) // 64
        capacity = old.capacity + (directory_blocks - old.directory_blocks)
        expect(case, "image.geometry",
               new.version == (1 if directory_blocks == 1 else 2)
               and new.directory_blocks == directory_blocks
               and new.entry_capacity == cfg["files"]
               and new.capacity == capacity
               and len(new.raw) == capacity * BLOCK,
               f"v{new.version} D={new.directory_blocks} "
               f"entries={new.entry_capacity} capacity={new.capacity} "
               f"expected D={directory_blocks} capacity={capacity}")
        old_by, new_by = old.by_slot, new.by_slot
        expect(case, "image.live-slots", set(old_by) == set(new_by),
               f"{sorted(new_by)} != {sorted(old_by)}")
        expect(case, "image.slot-names",
               all(new_by[slot].name == old_by[slot].name
                   for slot in old_by), "slot names differ")
        expect(case, "image.slot-sizes",
               all(new_by[slot].size == old_by[slot].size
                   for slot in old_by), "slot sizes differ")
        expect(case, "image.slot-bytes",
               all(new_by[slot].data == old_by[slot].data
                   for slot in old_by), "slot bytes differ")
        expect(case, "image.source-sha256",
               source_sha256(new) == source_sha256(old),
               f"{source_sha256(new)} != {source_sha256(old)}")
        structure = check_compact(new) + check_tail_padding(new)
        expect(case, "image.directory-structure", not structure,
               f"{structure}")

        target_assets = Path(str(target) + ".assets")
        if cfg["has_assets"]:
            actual = asset_manifest(target_assets)
            expect(case, "assets.canonical-manifest-equal",
                   actual == canonical,
                   f"expected {canonical} actual {actual}")
            expect(case, "assets.decoy-only-absent",
                   not any(record[0].startswith("decoy-only")
                           for record in (actual or [])),
                   f"decoy paths in {actual}")
        else:
            expect(case, "assets.target-absent",
                   not os.path.lexists(target_assets),
                   f"unexpected target assets {target_assets}")

        expect(case, "source.image-sha-unchanged",
               sha256_file(cfg["real"]) == image_sha, "source image changed")
        if cfg["has_assets"]:
            expect(case, "source.canonical-manifest-unchanged",
                   asset_manifest(cfg["canonical"]) == canonical,
                   "canonical assets changed")
        else:
            expect(case, "source.canonical-assets-still-absent",
                   not os.path.lexists(cfg["canonical"]),
                   "canonical assets appeared")
        expect(case, "source.decoy-manifest-unchanged",
               asset_manifest(cfg["decoy"]) == decoy, "decoy changed")
        expect(case, "alias.lstat-readlink-realpath-unchanged",
               alias_state(cfg["alias"]) == alias, "symlink state changed")

        canonical_lock = Path(os.path.realpath(str(cfg["real"])) + ".lock")
        expect(case, "lock.canonical-present",
               os.path.lexists(canonical_lock) and canonical_lock.is_file(),
               f"missing {canonical_lock}")
        alias_lock = Path(str(cfg["alias"]) + ".lock")
        expect(case, "lock.alias-absent", not os.path.lexists(alias_lock),
               f"unexpected {alias_lock}")
        side_locks = [str(path) for path in cfg["alias"].parent.glob("*.lock")]
        expect(case, "lock.no-alias-side-lock", not side_locks,
               f"unexpected {side_locks}")
        case["status"] = "PASS"
    except AssertionError as error:
        case["failed_assertion"] = str(error)
    except Exception as error:  # noqa: BLE001
        case["status"] = "ERROR"
        case["failed_assertion"] = f"{type(error).__name__}: {error}"
    return case


def write_report(run: Path, report: dict) -> Path:
    path = run / "report.json"
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Issue88 R7 symlink/decoy test")
    parser.add_argument("--migrate", help="real yan_migrate executable")
    parser.add_argument("--work", help="parent directory for a fresh run-*")
    parser.add_argument("--trace", action="store_true")
    args = parser.parse_args(argv)
    if not args.migrate:
        print("FATAL required artifact missing: --migrate", file=sys.stderr)
        return 1
    if not args.work:
        print("FATAL required artifact missing: --work", file=sys.stderr)
        return 1
    migrate = Path(os.path.abspath(args.migrate))
    if not migrate.is_file() or not os.access(str(migrate), os.X_OK):
        print(f"FATAL migrate input missing or not executable: {migrate}",
              file=sys.stderr)
        return 1
    work = Path(os.path.abspath(args.work))
    try:
        work.mkdir(parents=True, exist_ok=True)
        run = Path(tempfile.mkdtemp(prefix="run-", dir=str(work)))
    except OSError as error:
        print(f"FATAL cannot create a run directory under {work}: {error}",
              file=sys.stderr)
        return 1
    print(f"WORK={run}", flush=True)

    runner = Path(__file__).resolve()
    oracle_path = TESTS_DIR / "run_yanfs_migrate.py"
    report = {
        "runner": str(runner), "argv": list(sys.argv),
        "runner_sha256": sha256_file(runner),
        "oracle": {"path": str(oracle_path),
                   "sha256": sha256_file(oracle_path)},
        "migrate": {"input": args.migrate, "path": str(migrate),
                    "sha256": sha256_file(migrate)},
        "work": str(work), "run_dir": str(run),
        "cases": [], "summary": {},
    }
    try:
        cases = build_fixtures(run)
    except Exception as error:  # noqa: BLE001
        print(f"FATAL fixture construction failed: {error}", file=sys.stderr)
        if args.trace:
            traceback.print_exc()
        report["fixture_error"] = f"{type(error).__name__}: {error}"
        report["summary"] = {"cases": 0, "passed": 0, "failed": 0,
                             "checks": 0, "status": "FAIL"}
        report_path = write_report(run, report)
        print(f"REPORT={report_path}")
        return 1

    for cfg in cases:
        print(f"CASE {cfg['name']}", flush=True)
        case = run_case(cfg, run, migrate)
        for check in case["checks"]:
            print(f"  [PASS] {check['name']}", flush=True)
        if case["status"] != "PASS":
            print(f"  [{case['status']}] {case['failed_assertion']}",
                  flush=True)
        print(f"CASE {cfg['name']} [{AC1}] {case['status']}", flush=True)
        report["cases"].append(case)

    checks = [check for case in report["cases"] for check in case["checks"]]
    passed = [case for case in report["cases"] if case["status"] == "PASS"]
    report["summary"] = {
        "cases": len(report["cases"]), "passed": len(passed),
        "failed": len(report["cases"]) - len(passed),
        "checks": len(checks),
        "status": "PASS" if len(passed) == len(report["cases"]) else "FAIL",
    }
    report_path = write_report(run, report)
    summary = report["summary"]
    print(f"CASES={summary['cases']} PASS={summary['passed']} "
          f"FAIL={summary['failed']} CHECKS={summary['checks']}")
    print(f"WORK={run}")
    print(f"REPORT={report_path}")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
