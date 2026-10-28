#!/usr/bin/env python3
"""Explicit operator entry for the historical 0036-R8 cross-version comparison.

This is the issue #89 reproduction entry, not a default regression. It builds
both pinned refs from the local Git object database, runs the exact 33-case
matrix (24 terminal + 6 mount-corruption + 3 MCP), compares raw bytes and
image hashes, and writes an evidence manifest under --work.

Exit codes:
  0  the complete matrix passed at the pinned refs
  1  repository/ref/input problem or a comparison failure
  2  build/harness/timeout/sanitizer failure (never a behavior verdict)
 77  a required external tool or dependency is absent (NOT VERIFIED)

Never reads E: evidence, user images, or unrelated directories. It does not
fetch, check out, clean, or reuse a build. --work must be fresh.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
from types import SimpleNamespace

SCRIPT_REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SCRIPT_REPO / "tests"))
sys.path.insert(0, str(SCRIPT_REPO / "tests" / "guest"))

import run_terminal_runtime as runtime  # noqa: E402
import yanfs_r8_parity as parity  # noqa: E402

MAX_STEPS = 3_000_000_000
DEFAULT_TIMEOUT = 900
NAMED_INPUTS = (
    "tests/yanfs_r8_parity.py",
    "tests/guest/run_yanfs_r8_parity.py",
    "tests/guest/run_terminal_runtime.py",
    "tests/terminal_runtime_driver.c",
    "tests/mcp_checks.py",
    "tests/semantic_fixture.py",
    "tests/semantic_json_checks.py",
)

# The fixed historical refs predate the shared test-only arena observer. Keep
# one historical Host view and use it for both the member manifest check and
# the driver compile, so the current shared lists cannot require a file that
# did not exist at those refs.
HISTORICAL_HOST_SOURCES = tuple(
    item for item in runtime.HOST_SOURCES
    if item != "tests/arena_trace.c")
HISTORICAL_HOST_HEADERS = tuple(
    item for item in runtime.HOST_HEADERS
    if item != "tests/arena_trace.h")


class DependencyMissing(Exception):
    """An external tool/dependency is absent; the run is NOT VERIFIED."""


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="explicit historical 0036-R8 parity reproduction")
    parser.add_argument("--source", type=Path,
                        default=SCRIPT_REPO, help="repository root")
    parser.add_argument("--work", type=Path, required=True,
                        help="fresh output directory")
    parser.add_argument("--cc", required=True, help="host C compiler")
    parser.add_argument("--gcc", required=True, help="RISC-V cross compiler")
    parser.add_argument("--deps", type=Path, required=True,
                        help="prefix containing CURL and yyjson CMake packages")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                        help="per product process timeout in seconds")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def run_process(argv, timeout, data=None):
    try:
        return subprocess.run(argv, input=data, capture_output=True,
                              timeout=timeout, check=False)
    except FileNotFoundError as error:
        raise DependencyMissing(str(argv[0])) from error
    except subprocess.TimeoutExpired as error:
        raise runtime.HarnessError("timed out: " + " ".join(map(str, argv))) \
            from error


def write_bytes(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def atomic_json(path, value):
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=False)
        stream.write("\n")
    os.replace(tmp, path)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git(source, *args, timeout=300):
    return run_process(["git", "-C", str(source), *args], timeout)


def prepare_work(work, source):
    work = parity.check_fresh_work(work, source)
    if not work.exists():
        work.mkdir(parents=True)
    return work


def check_harness_identity(source):
    for relative, module in (("tests/yanfs_r8_parity.py", parity),
                             ("tests/guest/run_terminal_runtime.py", runtime)):
        expected = parity.file_sha256(source / relative)
        actual = parity.file_sha256(module.__file__)
        if expected != actual:
            raise parity.ParityFailure(
                "HARNESS_MISMATCH", "harness",
                "%s is not the module this entry imports" % relative)


def require_git_checkout(source):
    if not (source / ".git").exists():
        raise parity.ParityFailure("MISSING_INPUT", "inputs", ".git")
    if not runtime.tool_available("git"):
        raise DependencyMissing("git")


def resolve_refs(source):
    refs = {}
    for label, commit, tree in (
            ("baseline", parity.REF_BASELINE, parity.TREE_BASELINE),
            ("candidate", parity.REF_CANDIDATE, parity.TREE_CANDIDATE)):
        probe = git(source, "cat-file", "-e", commit + "^{commit}")
        if probe.returncode != 0:
            raise parity.ParityFailure("REF_MISSING", "ref", commit)
        actual = git(source, "rev-parse", commit + "^{commit}").stdout \
            .decode("utf-8", "replace").strip()
        if actual != commit:
            raise parity.ParityFailure("REF_MISMATCH", "ref",
                                       "%s != %s" % (actual, commit))
        actual_tree = git(source, "rev-parse", commit + "^{tree}").stdout \
            .decode("utf-8", "replace").strip()
        if actual_tree != tree:
            raise parity.ParityFailure("TREE_MISMATCH", "tree",
                                       "%s != %s" % (actual_tree, tree))
        refs[label] = {"commit": commit, "tree": tree}
    return refs


def check_dependencies(args):
    if not sys.platform.startswith("linux"):
        raise DependencyMissing("the historical parity entry is Linux/WSL only")
    for tool in (str(args.cc), str(args.gcc), "cmake", "git"):
        if not runtime.tool_available(tool):
            raise DependencyMissing(tool)
    if not args.deps.resolve().is_dir():
        raise DependencyMissing("CURL/yyjson prefix: %s" % args.deps)


def required_ref_inputs(label):
    guest = list(runtime.PRODUCTION_SOURCES)
    host = list(HISTORICAL_HOST_SOURCES)
    headers = list(HISTORICAL_HOST_HEADERS)
    # 0040 added these after both fixed refs; current production still requires them.
    later_headers = {"os/block_peek.h", "os/note_metadata_format_internal.h"}
    headers += [item for item in runtime.GUEST_HEADERS
                if item != "os/sort.h" and item not in later_headers]
    ordered = []
    for item in guest + host + headers + [runtime.GUEST_LD]:
        if item not in ordered:
            ordered.append(item)
    if label == "candidate":
        ordered.append("os/sort.h")
    return ordered


def validate_member_manifests(source, refs, manifest):
    for label, info in refs.items():
        members = git(source, "ls-tree", "-r", "--name-only",
                      info["commit"]).stdout.decode("utf-8", "replace").splitlines()
        member_set = set(members)
        required = required_ref_inputs(label)
        missing = [item for item in required if item not in member_set]
        if missing:
            raise parity.ParityFailure("MISSING_INPUT", "archive", missing[0])
        info.update({
            "member_count": len(members),
            "members_sha256": parity.sha256_hex(
                "\n".join(members).encode("utf-8")),
            "required_count": len(required),
            "required_inputs": required,
            "sort_header_required": label == "candidate",
            "sort_header_present": "os/sort.h" in member_set,
        })
    return refs


def extract_ref(source, work, ref, label, manifest):
    ref_dir = work / "refs" / label
    ref_dir.mkdir(parents=True)
    members = git(source, "ls-tree", "-r", "--name-only", ref).stdout \
        .decode("utf-8", "replace").splitlines()
    member_set = set(members)
    archive = git(source, "archive", "--format=tar", ref).stdout
    if not archive:
        raise parity.ParityFailure("ARCHIVE_EMPTY", "archive", label)
    root = ref_dir.resolve()
    try:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as bundle:
            for member in bundle:
                name = member.name
                path = Path(name)
                if path.is_absolute() or ".." in path.parts:
                    raise parity.ParityFailure("ARCHIVE_PATH", "archive", name)
                target = (ref_dir / path).resolve()
                if target != root and root not in target.parents:
                    raise parity.ParityFailure("ARCHIVE_PATH", "archive", name)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    stream = bundle.extractfile(member)
                    if stream is None:
                        raise runtime.HarnessError(
                            "archive member has no data: " + name)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(stream.read())
                elif member.issym() or member.islnk():
                    raise parity.ParityFailure("ARCHIVE_LINK", "archive", name)
                else:
                    continue
    except (tarfile.TarError, KeyError) as error:
        raise runtime.HarnessError("archive extraction failed: %s" % error) \
            from error
    required = required_ref_inputs(label)
    parity.require_named_inputs(ref_dir, required)
    missing = [item for item in required if item not in member_set]
    if missing:
        raise parity.ParityFailure("MISSING_INPUT", "archive", missing[0])
    manifest["refs"][label].update({
        "archive_sha256": sha256(archive),
        "member_count": len(members),
        "members_sha256": sha256("\n".join(members).encode("utf-8")),
        "inventory": {item: parity.file_sha256(ref_dir / item)
                      for item in required},
    })
    return ref_dir


def record_build(manifest, name, argv, result, stdout, stderr,
                 artifact=None, status="ok"):
    entry = {
        "command": [str(item) for item in argv],
        "returncode": result.returncode,
        "status": status,
        "stdout": str(stdout.relative_to(manifest["_work"])),
        "stderr": str(stderr.relative_to(manifest["_work"])),
        "stdout_sha256": parity.file_sha256(stdout),
        "stderr_sha256": parity.file_sha256(stderr),
        "artifact": None,
        "artifact_sha256": None,
    }
    if artifact is not None and Path(artifact).is_file():
        entry["artifact"] = str(Path(artifact).relative_to(manifest["_work"]))
        entry["artifact_sha256"] = parity.file_sha256(artifact)
    manifest["builds"][name] = entry
    return entry


def build_guest(ref_dir, work, gcc, label, manifest):
    artifact = work / "artifacts" / ("%s-yanfs_terminal.elf" % label)
    artifact.parent.mkdir(parents=True, exist_ok=True)
    argv = [str(gcc), *runtime.CROSS_FLAGS, "-I", str(ref_dir / "os"),
            "-T", str(ref_dir / "os" / "guest.ld")]
    argv += [str(ref_dir / item) for item in runtime.PRODUCTION_SOURCES]
    argv += ["-Wl,--build-id=none", "-o", str(artifact)]
    result = run_process(argv, 600)
    stdout = work / "logs" / ("%s-guest-build.stdout" % label)
    stderr = work / "logs" / ("%s-guest-build.stderr" % label)
    write_bytes(stdout, result.stdout)
    write_bytes(stderr, result.stderr)
    if result.returncode != 0:
        status = "build_failed"
    elif not artifact.is_file():
        status = "artifact_missing"
    else:
        status = "ok"
    record_build(manifest, "guest-" + label, argv, result, stdout, stderr,
                 artifact, status)
    if status != "ok":
        raise runtime.HarnessError("Guest build failed (%s):\n%s" %
                                   (label, result.stderr.decode("utf-8", "replace")))
    return artifact


def build_driver(candidate_tree, work, cc, manifest):
    out_dir = work / "build" / "driver"
    out_dir.mkdir(parents=True)
    driver = out_dir / "terminal_runtime_driver"
    argv = [str(cc), *runtime.HOST_CC_FLAGS,
            "-I", str(candidate_tree / "include"),
            "-I", str(candidate_tree / "tools")]
    argv += [str(candidate_tree / item)
             for item in HISTORICAL_HOST_SOURCES]
    argv += ["-o", str(driver)]
    result = run_process(argv, 600)
    stdout = work / "logs" / "driver-build.stdout"
    stderr = work / "logs" / "driver-build.stderr"
    write_bytes(stdout, result.stdout)
    write_bytes(stderr, result.stderr)
    if result.returncode != 0:
        status = "build_failed"
    elif not driver.is_file():
        status = "artifact_missing"
    else:
        status = "ok"
    record_build(manifest, "driver", argv, result, stdout, stderr, driver,
                 status)
    if status != "ok":
        raise runtime.HarnessError("driver build failed:\n%s" %
                                   result.stderr.decode("utf-8", "replace"))
    return driver


def build_mcp(ref_dir, work, deps, label, manifest):
    build_dir = work / "build" / ("mcp-" + label)
    build_dir.mkdir(parents=True)
    configure = ["cmake", "-S", str(ref_dir), "-B", str(build_dir),
                 "-DBUILD_TESTING=OFF", "-DYAN_BUILD_TOOLS=OFF",
                 "-DYAN_BUILD_SEMANTIC=ON", "-DCMAKE_BUILD_TYPE=Release",
                 "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                 "-DCMAKE_PREFIX_PATH=" + str(deps)]
    result = run_process(configure, 600)
    config_out = work / "logs" / ("%s-mcp-configure.stdout" % label)
    config_err = work / "logs" / ("%s-mcp-configure.stderr" % label)
    write_bytes(config_out, result.stdout)
    write_bytes(config_err, result.stderr)
    dependency_failure = False
    if result.returncode != 0:
        text = (result.stdout + result.stderr).decode("utf-8", "replace").lower()
        dependency_failure = (
            "could not find a package configuration file" in text and
            ("curl" in text or "yyjson" in text))
    configure_status = ("ok" if result.returncode == 0 else
                        ("dependency_missing" if dependency_failure
                         else "configure_failed"))
    record_build(manifest, "mcp-" + label + "-configure", configure, result,
                 config_out, config_err, None, configure_status)
    if result.returncode != 0:
        if dependency_failure:
            raise DependencyMissing("CURL/yyjson CMake package under %s" % deps)
        raise runtime.HarnessError("MCP configure failed (%s):\n%s" %
                                   (label, result.stderr.decode("utf-8", "replace")))
    compile_argv = ["cmake", "--build", str(build_dir), "--target", "yan_mcp",
                    "--parallel"]
    built = run_process(compile_argv, 900)
    build_out = work / "logs" / ("%s-mcp-build.stdout" % label)
    build_err = work / "logs" / ("%s-mcp-build.stderr" % label)
    write_bytes(build_out, built.stdout)
    write_bytes(build_err, built.stderr)
    server = build_dir / "yan_mcp"
    if built.returncode != 0:
        build_status = "build_failed"
    elif not server.is_file():
        build_status = "artifact_missing"
    else:
        build_status = "ok"
    record_build(manifest, "mcp-" + label, compile_argv, built,
                 build_out, build_err, server, build_status)
    if build_status != "ok":
        if build_status == "build_failed":
            raise runtime.HarnessError("MCP build failed (%s):\n%s" %
                                       (label, built.stderr.decode("utf-8", "replace")))
        raise runtime.HarnessError("yan_mcp was not produced: " + str(server))
    server_sha = parity.file_sha256(server)
    cache = build_dir / "CMakeCache.txt"
    cache_text = cache.read_text(encoding="utf-8", errors="replace") \
        if cache.is_file() else ""
    manifest["builds"]["mcp-" + label].update({
        "artifact_sha256": server_sha,
        "cache_curl": _cache_line(cache_text, "CURL_DIR"),
        "cache_yyjson": _cache_line(cache_text, "yyjson_DIR"),
    })
    return server


def _cache_line(text, key):
    prefix = key + ":"
    for line in text.splitlines():
        if line.startswith(prefix):
            return line
    return None


def make_fixtures(work, manifest):
    fixtures = {}
    fixture_dir = work / "fixtures"
    fixture_dir.mkdir(parents=True)
    for files in parity.SIZES:
        image = parity.encode(files)
        info = parity.validate_fixture(files, image)
        path = fixture_dir / ("filled-%d.img" % files)
        write_bytes(path, image)
        manifest["fixtures"][str(files)] = {
            "path": str(path.relative_to(manifest["_work"])),
            "sha256": info["image_sha256"],
            "entries_sha256": info["entries_sha256"],
            "names_sha256": info["names_sha256"],
            "directory_blocks": info["directory_blocks"],
            "capacity_blocks": info["capacity_blocks"],
            "plain_slot": info["plain_slot"],
            "plain_name": info["plain_name"],
            "contiguous": True,
            "name_order_permuted": True,
        }
        fixtures[files] = image
    for files in parity.SIZES:
        image = fixtures[files]
        for defect, mutate in (("duplicate", parity.corrupt_duplicate),
                               ("overlap", parity.corrupt_overlap)):
            corrupted = mutate(image)
            path = fixture_dir / ("corrupt-%d-%s.img" % (files, defect))
            write_bytes(path, corrupted)
            manifest["fixtures"]["%d-%s" % (files, defect)] = {
                "path": str(path.relative_to(manifest["_work"])),
                "sha256": sha256(corrupted),
                "defect": defect,
            }
    return fixtures


def run_terminal_side(driver, elf, image, stdin, files, case, side,
                      expect_tohost, timeout, work):
    label = "%d-%s-%s" % (files, case, side)
    namespace = SimpleNamespace(guest=str(elf))
    result, status_path, disk, capture = runtime.run_production(
        driver, namespace, work / "runs", image, stdin, label, "none", 0, 0,
        expect_tohost=expect_tohost, max_steps=MAX_STEPS, timeout=timeout)
    values = runtime.classify_production(result, status_path, label,
                                         {expect_tohost})
    case_dir = work / "runs" / ("production-" + label)
    observation = parity.TerminalObservation(
        capture.read_bytes(), result.returncode, values, image,
        disk.read_bytes(), result.stdout, result.stderr)
    return observation, case_dir, label


def observation_record(observation, case_dir, label):
    paths = {suffix: case_dir / (label + suffix)
             for suffix in (".cmd", ".stdout", ".stderr", ".status",
                            ".out", ".img", ".stdin")}
    return {
        "exit": observation.exit,
        "verdict": observation.status.get("verdict"),
        "tohost": observation.status.get("tohost"),
        "out_length": observation.status.get("out_length"),
        "status": observation.status,
        "command": paths[".cmd"].read_text(encoding="utf-8").strip(),
        "stdin_sha256": parity.file_sha256(paths[".stdin"]),
        "capture_sha256": sha256(observation.capture),
        "capture_bytes": len(observation.capture),
        "stdout_sha256": parity.file_sha256(paths[".stdout"]),
        "stderr_sha256": parity.file_sha256(paths[".stderr"]),
        "status_sha256": parity.file_sha256(paths[".status"]),
        "image_before_sha256": sha256(observation.image_before),
        "image_after_sha256": sha256(observation.image_after),
    }


def run_terminal_cases(driver, guest, fixtures, work, args, manifest):
    for files, case in parity.case_matrix():
        if case in parity.CORRUPT_CASES or case == parity.MCP_CASE:
            continue
        image = fixtures[files]
        expected = parity.expected_terminal(files, case)
        left, left_dir, left_label = run_terminal_side(
            driver, guest["baseline"], image, expected["stdin"], files, case,
            "baseline", 1, args.timeout, work)
        right, right_dir, right_label = run_terminal_side(
            driver, guest["candidate"], image, expected["stdin"], files, case,
            "candidate", 1, args.timeout, work)
        parity.compare_terminal(left, right, expected, image)
        manifest["cases"].append({
            "id": "%d:%s" % (files, case), "files": files, "case": case,
            "kind": "terminal",
            "baseline": observation_record(left, left_dir, left_label),
            "candidate": observation_record(right, right_dir, right_label),
            "equal": True,
            "expected": {
                "receipt": expected["receipt"].decode("ascii").rstrip("\r\n"),
                "stdin_sha256": sha256(expected["stdin"]),
                "body_sha256": sha256(expected["body"]),
                "plain_name": expected["plain_name"],
                "related_names": expected["related_names"],
                "block_names": expected["block_names"],
                "search_total": expected["search_total"],
            },
        })
        print("PASS %d %s" % (files, case), flush=True)


def run_corrupt_cases(driver, guest, fixtures, work, args, manifest):
    for files, defect in [(files, defect) for files in parity.SIZES
                          for defect in parity.CORRUPT_CASES]:
        image = fixtures[files]
        corrupted = (parity.corrupt_duplicate(image) if defect == "duplicate"
                     else parity.corrupt_overlap(image))
        expected = parity.expected_corrupt(files)
        left, left_dir, left_label = run_terminal_side(
            driver, guest["baseline"], corrupted, expected["stdin"], files,
            defect, "baseline", parity.CORRUPT_TOHOST, args.timeout, work)
        right, right_dir, right_label = run_terminal_side(
            driver, guest["candidate"], corrupted, expected["stdin"], files,
            defect, "candidate", parity.CORRUPT_TOHOST, args.timeout, work)
        parity.compare_corrupt(left, right, corrupted)
        manifest["cases"].append({
            "id": "%d:%s" % (files, defect), "files": files, "case": defect,
            "kind": "corrupt",
            "baseline": observation_record(left, left_dir, left_label),
            "candidate": observation_record(right, right_dir, right_label),
            "equal": True,
            "expected": {"capture": expected["capture"].decode("ascii").rstrip("\r\n"),
                         "exit": expected["exit"],
                         "tohost": "0x%08x" % expected["tohost"]},
        })
        print("PASS %d %s" % (files, defect), flush=True)


def check_mcp_completion(returncode, extra, stderr):
    """Check the entire process outcome, including diagnostics at shutdown."""
    if b"sanitizer" in stderr.lower() or b"runtime error:" in stderr.lower():
        raise runtime.HarnessError("MCP sanitizer diagnostic")
    if returncode != 0:
        raise runtime.HarnessError("MCP server exit %s" % returncode)
    if extra:
        raise runtime.HarnessError("MCP unexpected trailing output")


def mcp_tool_value(tool_value, reply, code):
    """Narrow schema boundary around the shared mcp_checks helper."""
    try:
        if code is None:
            tool_value(reply)
        else:
            tool_value(reply, code=code)
    except (AssertionError, KeyError, TypeError, IndexError, ValueError) as error:
        raise parity.ParityFailure("MCP_SCHEMA", "reply", str(error)) from error


def _mcp_evidence(work, files, side, server_args, raw, partial, stderr,
                  exit_code, error_text):
    case_dir = work / "cases" / ("mcp-%d-%s" % (files, side))
    case_dir.mkdir(parents=True, exist_ok=True)
    write_bytes(case_dir / "server.argv.json",
                (json.dumps(server_args) + "\n").encode("utf-8"))
    write_bytes(case_dir / "response.line", raw + (b"\n" if raw else b""))
    write_bytes(case_dir / "partial.buffer", partial)
    write_bytes(case_dir / "stderr.bin", stderr)
    write_bytes(case_dir / "error.txt", error_text)
    write_bytes(case_dir / "exit.txt", ("%s\n" % exit_code).encode("ascii"))
    return case_dir


def run_mcp_side(server, image_path, image, files, side, expected, work,
                 source):
    sys.path.insert(0, str(source / "tests"))
    from mcp_checks import Wire, tool_value  # lazy: Linux MCP path only

    server_args = [str(server), "--disk-image", str(image_path),
                   "--index", str(work / "absent-index"),
                   "--model", str(work / "absent-model"),
                   "--runner", str(work / "absent-runner"),
                   "--startup-timeout", "3", "--request-timeout", "30"]
    wire = Wire(server_args, os.environ.copy())
    raw = b""
    reply = None
    failure = None
    partial = b""
    stderr = b""
    exit_code = None
    stderr_from_error = b""
    error_text = b""
    try:
        wire.send("tools/call", params={
            "name": "yanos_block_backlinks",
            "arguments": {"name": "f0000.txt", "block_id": "x"}})
        raw = wire.read_line(60)
        try:
            reply = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise parity.ParityFailure("MCP_JSON", "raw", str(error)) from error
        mcp_tool_value(tool_value, reply,
                       None if expected["mode"] == "success" else "CORRUPT")
    except RuntimeError as error:
        failure = runtime.HarnessError("MCP harness: %s" % error)
        error_text = ("MCP harness: %s" % error).encode("utf-8", "replace")
        argument = error.args[0] if error.args else None
        if isinstance(argument, tuple) and len(argument) >= 3 and \
                isinstance(argument[2], bytes):
            stderr_from_error = argument[2]
    except AssertionError as error:
        failure = runtime.HarnessError("MCP harness assertion: %s" % error)
        error_text = ("MCP harness assertion: %s" % error).encode(
            "utf-8", "replace")
    except parity.ParityFailure as error:
        failure = error
        error_text = ("%s [%s]: %s" % (error.code, error.field,
                                       error.detail)).encode("utf-8", "replace")
    except OSError as error:
        failure = runtime.HarnessError("MCP I/O: %s" % error)
        error_text = ("MCP I/O: %s" % error).encode("utf-8", "replace")
    finally:
        partial = getattr(wire, "buffer", b"")
        # Read stderr once, through EOF. Draining it before Wire.close would
        # hide early sanitizer diagnostics from that helper and lose late logs.
        try:
            if isinstance(failure, runtime.HarnessError) and wire.p.poll() is None:
                wire.p.kill()
            if wire.p.stdin is not None:
                try:
                    wire.p.stdin.close()
                finally:
                    wire.p.stdin = None
            try:
                tail, remaining_stderr = wire.p.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                wire.p.kill()
                try:
                    tail, remaining_stderr = wire.p.communicate(timeout=5)
                except subprocess.TimeoutExpired as error:
                    tail = error.output or b""
                    remaining_stderr = error.stderr or b""
                    wire.p.stdout.close()
                    wire.p.stderr.close()
                failure = runtime.HarnessError("MCP shutdown timed out")
            partial += tail
            stderr = stderr_from_error + remaining_stderr
        except OSError as error:
            failure = runtime.HarnessError("MCP shutdown I/O: %s" % error)
            if wire.p.poll() is None:
                wire.p.kill()
            wire.p.wait(timeout=5)
            stderr = stderr_from_error
        exit_code = wire.p.returncode
        try:
            check_mcp_completion(exit_code, partial, stderr)
        except runtime.HarnessError as error:
            if not isinstance(failure, runtime.HarnessError):
                failure = error
        if failure is not None:
            error_text = str(failure).encode("utf-8", "replace")
        case_dir = _mcp_evidence(work, files, side, server_args, raw, partial,
                                 stderr, exit_code, error_text)
    if failure is not None:
        raise failure
    after = image_path.read_bytes()
    observation = parity.McpObservation(raw, reply, int(exit_code), image,
                                        after)
    evidence = {
        "server_args": server_args,
        "case_dir": case_dir,
        "response": case_dir / "response.line",
        "partial": case_dir / "partial.buffer",
        "stderr": case_dir / "stderr.bin",
        "error": case_dir / "error.txt",
        "exit_file": case_dir / "exit.txt",
    }
    return observation, evidence


def run_mcp_cases(servers, fixtures, work, manifest):
    source = manifest["_source"]
    sys.path.insert(0, str(source / "tests"))
    import mcp_checks  # noqa: E402
    if parity.file_sha256(source / "tests/mcp_checks.py") != \
            parity.file_sha256(mcp_checks.__file__):
        raise parity.ParityFailure("HARNESS_MISMATCH", "harness", "mcp_checks.py")
    for files in parity.SIZES:
        expected = parity.expected_mcp(files)
        image = fixtures[files]
        image_path = work / "fixtures" / ("filled-%d.img" % files)
        left, left_evidence = run_mcp_side(
            servers["baseline"], image_path, image, files, "baseline",
            expected, work, source)
        right, right_evidence = run_mcp_side(
            servers["candidate"], image_path, image, files, "candidate",
            expected, work, source)
        parsed = parity.compare_mcp(left, right, expected, image)

        def evidence_record(observation, evidence):
            return {
                "exit": observation.exit,
                "wire_sha256": sha256(observation.raw),
                "wire_bytes": len(observation.raw),
                "wire": str(evidence["response"].relative_to(work)),
                "partial": str(evidence["partial"].relative_to(work)),
                "stderr": str(evidence["stderr"].relative_to(work)),
                "error": str(evidence["error"].relative_to(work)),
                "exit_file": str(evidence["exit_file"].relative_to(work)),
                "server_args": evidence["server_args"],
            }

        if expected["mode"] == "success":
            summary = {"total": parsed["result"]["structuredContent"]["total"],
                       "shown": parsed["result"]["structuredContent"]["shown"],
                       "target_state": parsed["result"]["structuredContent"]["target_state"],
                       "sources": [entry["source"] for entry in
                                   parsed["result"]["structuredContent"]["backlinks"]]}
        else:
            summary = {"code": json.loads(
                parsed["result"]["content"][0]["text"])["code"]}
        manifest["cases"].append({
            "id": "%d:mcp" % files, "files": files, "case": "mcp",
            "kind": "mcp", "mode": expected["mode"],
            "equal": True, "parsed": summary,
            "baseline": evidence_record(left, left_evidence),
            "candidate": evidence_record(right, right_evidence),
            "image_sha256": sha256(image),
        })
        print("PASS %d mcp (%s)" % (files, expected["mode"]), flush=True)


def tool_versions(args):
    versions = {}
    for key, argv in (("cc", [str(args.cc), "--version"]),
                      ("gcc", [str(args.gcc), "--version"]),
                      ("cmake", ["cmake", "--version"]),
                      ("git", ["git", "--version"])):
        result = run_process(argv, 60)
        versions[key] = result.stdout.decode("utf-8", "replace").splitlines()[0] \
            if result.stdout else "(empty)"
    versions["python"] = sys.version.splitlines()[0]
    return versions


def build_manifest(source, args):
    work = args.work.resolve()
    tool_path = Path(__file__).resolve()
    manifest = {
        "schema": 1,
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "status": "running",
        "tool": {"path": str(tool_path),
                 "sha256": parity.file_sha256(tool_path)},
        "source": str(source),
        "work": str(work),
        "head": None,
        "current_main_compared": False,
        "v2_error_parity": "expected error parity only; v2 retrieval is not successful",
        "matrix": {"pair_cases": 33, "terminal": 24, "corrupt": 6, "mcp": 3,
                   "product_executions": 66},
        "refs": {}, "artifacts": {}, "builds": {}, "fixtures": {},
        "cases": [], "summary": {},
        "_work": work, "_source": source,
    }
    return manifest


def public_manifest(manifest):
    return {key: value for key, value in manifest.items()
            if not key.startswith("_")}


def write_failure(manifest, work, prepared, status, code, field, detail):
    if not prepared:
        return
    manifest["status"] = status
    manifest["error"] = {"code": code, "field": field, "detail": detail}
    manifest["summary"] = {"completed": len(manifest["cases"]), "partial": True}
    try:
        atomic_json(work / "manifest.json", public_manifest(manifest))
    except OSError:
        pass


def main(argv=None):
    args = parse_args(argv)
    source = args.source.resolve()
    manifest = build_manifest(source, args)
    work = args.work.resolve()
    prepared = False
    try:
        parity.require_named_inputs(source, NAMED_INPUTS)
        if not (source / ".git").exists():
            raise parity.ParityFailure("MISSING_INPUT", "inputs", ".git")
        check_harness_identity(source)
        require_git_checkout(source)
        refs = resolve_refs(source)
        manifest["refs"] = refs
        validate_member_manifests(source, refs, manifest)
        prepare_work(work, source)
        prepared = True
        check_dependencies(args)
        head = git(source, "rev-parse", "HEAD").stdout.decode("utf-8", "replace").strip()
        manifest["head"] = head
        manifest["toolchain"] = tool_versions(args)
        manifest["deps"] = str(args.deps.resolve())
        baseline_tree = extract_ref(source, work, parity.REF_BASELINE,
                                    "baseline", manifest)
        candidate_tree = extract_ref(source, work, parity.REF_CANDIDATE,
                                     "candidate", manifest)
        fixtures = make_fixtures(work, manifest)
        guest = {
            "baseline": build_guest(baseline_tree, work, args.gcc,
                                    "baseline", manifest),
            "candidate": build_guest(candidate_tree, work, args.gcc,
                                     "candidate", manifest),
        }
        manifest["artifacts"]["guest"] = {
            key: {"path": str(path.relative_to(work)),
                  "sha256": parity.file_sha256(path)}
            for key, path in guest.items()}
        driver = build_driver(candidate_tree, work, args.cc, manifest)
        manifest["artifacts"]["driver"] = {
            "path": str(driver.relative_to(work)),
            "sha256": parity.file_sha256(driver)}
        servers = {
            "baseline": build_mcp(baseline_tree, work, args.deps.resolve(),
                                  "baseline", manifest),
            "candidate": build_mcp(candidate_tree, work, args.deps.resolve(),
                                   "candidate", manifest),
        }
        manifest["artifacts"]["mcp"] = {
            key: {"path": str(path.relative_to(work)),
                  "sha256": parity.file_sha256(path)}
            for key, path in servers.items()}
        run_terminal_cases(driver, guest, fixtures, work, args, manifest)
        run_corrupt_cases(driver, guest, fixtures, work, args, manifest)
        run_mcp_cases(servers, fixtures, work, manifest)
        parity.assert_case_matrix(
            [(case["files"], case["case"]) for case in manifest["cases"]])
        if len(manifest["cases"]) != 33:
            raise parity.ParityFailure("MATRIX_LENGTH", "matrix",
                                       "zero/incomplete matrix")
        manifest["status"] = "pass"
        manifest["summary"] = {
            "pair_cases": len(manifest["cases"]),
            "terminal": sum(1 for case in manifest["cases"]
                            if case["kind"] == "terminal"),
            "corrupt": sum(1 for case in manifest["cases"]
                           if case["kind"] == "corrupt"),
            "mcp": sum(1 for case in manifest["cases"]
                       if case["kind"] == "mcp"),
            "product_executions": 2 * len(manifest["cases"]),
            "pass": len(manifest["cases"]),
            "fail": 0,
            "partial": False,
        }
        atomic_json(work / "manifest.json", public_manifest(manifest))
        print("PASS yanfs_r8_parity: 33 cases / 66 executions; "
              "24 terminal + 6 corrupt + 3 MCP; v2=expected error parity",
              flush=True)
        return 0
    except parity.ParityFailure as error:
        write_failure(manifest, work, prepared, "fail", error.code,
                      error.field, error.detail)
        print("FAIL %s [%s]: %s" % (error.code, error.field, error.detail),
              flush=True)
        return 1
    except DependencyMissing as error:
        write_failure(manifest, work, prepared, "not_verified", "DEPENDENCY",
                      "dependency", str(error))
        print("NOT VERIFIED: missing external dependency: %s" % error,
              flush=True)
        return 77
    except runtime.OwnerFailure as error:
        write_failure(manifest, work, prepared, "fail", "OWNER_FAILURE",
                      "case", str(error))
        print("FAIL OWNER_FAILURE [case]: %s" % error, flush=True)
        return 1
    except (runtime.HarnessError, RuntimeError,
            subprocess.SubprocessError, OSError) as error:
        write_failure(manifest, work, prepared, "fail", "HARNESS", "harness",
                      str(error))
        print("HARNESS-ERROR: %s" % error, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
