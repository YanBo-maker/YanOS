#!/usr/bin/env python3
"""YanFS mutation gate: does the test suite actually detect real defects?

What this does
--------------
It copies a minimal tree out of the repository into a scratch directory
(os/yanfs.{h,c}, os/yanfs_block.{h,c} and the headers needed by the adapter TU,
tests/test_yanfs.c, tests/test_yanfs_adapter.c and the host Unity sources),
builds both unit suites with the repository's strict C17 warning bar
(-Wall -Wextra -Wpedantic -Werror), runs them, then applies one exact,
single-occurrence source replacement per mutation and repeats the build/run.
A mutation counts as *detected* only when the owning test named in
OWNING_ASSERTIONS fails:

    detected      the owner test reports :FAIL: and no harness-level condition
                  (signal, timeout, sanitizer, fixture abort, build failure,
                  Unity summary disagreement) is present
    survivor      the mutant run still passes
    wrong-owner   some test fails, but not the declared owner
    harness-error build failure, crash, signal, timeout, sanitizer, abort, or an
                  exit code and Unity summary that disagree

Only "detected" is a pass. A survivor, a wrong-owner result or a harness error
makes the gate exit non-zero, so a mutation can never be credited by an
unrelated failure. The baseline and two real negative controls (a no-effect
replacement that must survive, and a wrong-owner table entry that must not be
credited) run before the mutants.

Exit codes: 0 all detected, 1 survivor/wrong-owner, 2 harness error or a failed
baseline/control, 77 the external C compiler or Unity is missing.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

STRICT_FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
MUTANT_FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic"]
BUILD_TIMEOUT = 180
RUN_TIMEOUT = 90

OS_FILES = ["yanfs.h", "yanfs.c", "yanfs_block.h", "yanfs_block.c",
            "block.h", "platform.h", "task.h"]
TEST_FILES = ["test_yanfs.c", "test_yanfs_adapter.c"]

# mutation name -> (owning Unity test function, required assertion marker)
# The marker is the exact FAIL message the intended owning assertion prints in
# the verified run; a same-owner failure on any other assertion does not match.
OWNING_ASSERTIONS = {
    "bad_crc_accepted":
        ("mount_rejects_wrong_crc_unknown_magic_and_wrong_sizes",
         "YFS bad_crc accepted"),
    "extent_overlap_accepted":
        ("mount_checks_partial_extent_overlap_and_adjacency",
         "YFS extent overlap accepted"),
    "tail_not_zeroed":
        ("read_handles_all_lengths_and_tail_zero",
         "the bytes past the logical end must be zero"),
    "cache_published_before_validation":
        ("mount_never_writes_and_publishes_cache_only_after_validation",
         "YFS cache published early"),
    "faulted_cache_still_read":
        ("faulted_instance_refuses_cached_views", "YFS faulted read allowed"),
    "first_fit_ignores_fragmentation":
        ("create_reports_nospace_for_fragmented_free_space",
         "two free blocks that are not adjacent cannot hold a two-block file"),
    "error_frame_waits_for_payload":
        ("read_device_error_is_io_without_waiting",
         "the adapter must not wait on a complete 16-byte error frame"),
    "wrong_tag_accepted":
        ("wrong_tag_full_frame_is_consumed_then_rejected",
         "YFS wrong tag accepted"),
    "extent_start_range_accepted":
        ("mount_rejects_out_of_range_and_overlapping_extents",
         "YFS extent start accepted"),
    "replace_borrows_old_extent":
        ("replace_cannot_borrow_its_own_extent_at_peak",
         "a replacement cannot reuse the extent it is replacing"),
    "rename_changes_extent":
        ("rename_moves_name_in_original_slot_with_single_metadata_write",
         "a rename must keep the original extent start"),
    "self_rename_rewrites_metadata":
        ("rename_same_name_is_zero_io_even_with_full_directory_and_disk",
         "a same-name rename must not write the device, even with a full directory"),
    "rename_pointer_equal_shortcut":
        ("rename_validates_both_names_before_source_existence",
         "a missing source must not succeed as a same-name rename"),
    "copy_pointer_equal_shortcut":
        ("copy_validates_both_names_before_source_existence",
         "a missing source must not report EXISTS for a same-name copy"),
    "copy_borrows_source_extent":
        ("copy_reports_nospace_for_fragmented_free_space",
         "a copy must not borrow or split a non-contiguous free extent"),
    "copy_skips_final_tail_zero":
        ("copy_zeroes_target_tail_while_source_padding_survives",
         "the copy must zero its tail even when the source padding is non-zero"),
    "copy_publishes_candidate_before_write":
        ("copy_metadata_write_failure_keeps_old_cache",
         "a failed directory write must keep the previous directory in the cache"),
    "copy_reads_neighbour_block":
        ("copy_allocates_first_slot_and_low_first_fit_extent",
         "the copy's first data block must hold the source bytes"),
    "copy_ignores_source_read_error":
        ("copy_read_failure_faults_without_writing_metadata",
         "a source read error must surface as YAN_FS_IO"),
}

MUTATIONS = [
    {
        "name": "bad_crc_accepted",
        "file": "os/yanfs.c",
        "old": """    if (load_le32(block + FS_CRC_OFFSET) != yan_fs_metadata_crc(block)) {
        return YAN_FS_CORRUPT;
    }
""",
        "new": """    if ((load_le32(block + FS_CRC_OFFSET) ^ yan_fs_metadata_crc(block)) == 0u) {
        (void)0;
    }
""",
    },
    {
        "name": "extent_overlap_accepted",
        "file": "os/yanfs.c",
        "old": """            if (first_start < second_start + second_count &&
                second_start < first_start + first_count) {
                return YAN_FS_CORRUPT; /* extents must not overlap */
            }
""",
        "new": """            if (first_start == 0u && second_start == 0u &&
                first_start < second_start + second_count &&
                second_start < first_start + first_count) {
                return YAN_FS_CORRUPT; /* extents must not overlap */
            }
""",
    },
    {
        "name": "tail_not_zeroed",
        "file": "os/yanfs.c",
        "old": "            fs->scratch[j] = j < chunk ? bytes[offset + j] : 0u;\n",
        "new": "            fs->scratch[j] = j < chunk ? bytes[offset + j] : 0xA5u;\n",
    },
    {
        "name": "cache_published_before_validation",
        "file": "os/yanfs.c",
        "old": """            result = validate_metadata(fs->scratch, (uint32_t)device_blocks);
            if (result == YAN_FS_OK) {
""",
        "new": """            for (uint32_t i = 0; i < YAN_FS_BLOCK_SIZE; ++i) {
                fs->metadata[i] = fs->scratch[i];
            }
            result = validate_metadata(fs->scratch, (uint32_t)device_blocks);
            if (result == YAN_FS_OK) {
""",
    },
    {
        "name": "faulted_cache_still_read",
        "file": "os/yanfs.c",
        "old": """    if (fs->busy) {
        return YAN_FS_BUSY;
    }
    if (fs->state == YAN_FS_STATE_FAULTED) {
        return YAN_FS_FAULTED;
    }
    if (fs->state != YAN_FS_MOUNTED) {
        return YAN_FS_NOT_MOUNTED;
    }
""",
        "new": """    if (fs->busy) {
        return YAN_FS_BUSY;
    }
    if (fs->state == YAN_FS_STATE_FAULTED) {
        return YAN_FS_OK;
    }
    if (fs->state != YAN_FS_MOUNTED) {
        return YAN_FS_NOT_MOUNTED;
    }
""",
    },
    {
        "name": "first_fit_ignores_fragmentation",
        "file": "os/yanfs.c",
        "old": """        uint32_t limit = fs->capacity_blocks;
        for (uint32_t i = 0; i < used_total; ++i) {
            if (used_start[i] > candidate && used_start[i] < limit) {
                limit = used_start[i];
            }
        }
        if (limit - candidate >= needed && (best == 0u || candidate < best)) {
""",
        "new": """        uint32_t limit = fs->capacity_blocks;
        if (limit - candidate >= needed && (best == 0u || candidate < best)) {
""",
    },
    {
        "name": "error_frame_waits_for_payload",
        "file": "os/yanfs_block.c",
        "old": """            if (count != 0u) {
                return 1; /* a failure carries no data */
            }
            payload = 0u;
""",
        "new": """            if (count != 0u) {
                return 1; /* a failure carries no data */
            }
            payload = YAN_OS_BLOCK_BLOCK_SIZE;
""",
    },
    {
        "name": "wrong_tag_accepted",
        "file": "os/yanfs_block.c",
        "old": """    if (response.op != adapter->pending_op ||
        response.tag != adapter->pending_tag ||
        response.lba != adapter->pending_lba) {
""",
        "new": """    if (response.op != adapter->pending_op ||
        response.lba != adapter->pending_lba) {
""",
    },
    {
        "name": "extent_start_range_accepted",
        "file": "os/yanfs.c",
        "old": """        /* 0021: confirm 1 <= start < capacity first, then the count fits. */
        if (start_block < 1u || start_block >= capacity_blocks) {
            return YAN_FS_CORRUPT;
        }
        if (block_count > capacity_blocks - start_block) {
            return YAN_FS_CORRUPT;
        }
""",
        "new": """        /* mutant: the start range check is dropped. */
        if (block_count > capacity_blocks - start_block) {
            return YAN_FS_CORRUPT;
        }
""",
    },
    {
        "name": "replace_borrows_old_extent",
        "file": "os/yanfs.c",
        "old": """    /* The old extent is part of the current directory, so it is in the in-use
     * set and cannot be borrowed: the new data needs its own room. */
    if (needed > 0u && !allocate_extent(fs, needed, &start_block)) {
        return YAN_FS_NOSPACE;
    }
""",
        "new": """    /* mutant: fall back to the extent being replaced instead of allocating. */
    if (needed > 0u) {
        start_block = load_le32(previous + FS_ENTRY_START_OFFSET);
    }
""",
    },
    {
        "name": "rename_changes_extent",
        "file": "os/yanfs.c",
        "old": "              size_bytes, start_block, block_count);\n",
        "new": "              size_bytes, start_block + 1u, block_count);\n",
    },
    {
        "name": "self_rename_rewrites_metadata",
        "file": "os/yanfs.c",
        "old": """        if (target == slot) {
            return YAN_FS_OK; /* the same name is a no-op, not a write */
        }
""",
        "new": """        if (target == slot) {
            /* mutant: rewrite the directory even for a self rename */
            for (uint32_t i = 0; i < YAN_FS_BLOCK_SIZE; ++i) {
                fs->scratch[i] = fs->metadata[i];
            }
            return commit_metadata(fs);
        }
""",
    },
    {
        "name": "rename_pointer_equal_shortcut",
        "file": "os/yanfs.c",
        "old": """    uint32_t slot = 0;
    if (!find_entry(fs, old_name, old_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
""",
        "new": """    if (old_name == new_name) {
        return YAN_FS_OK; /* mutant: a pointer-equal shortcut before the lookup */
    }
    uint32_t slot = 0;
    if (!find_entry(fs, old_name, old_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
""",
    },
    {
        "name": "copy_pointer_equal_shortcut",
        "file": "os/yanfs.c",
        "old": """    uint32_t source_slot = 0;
    if (!find_entry(fs, source_name, source_length, &source_slot)) {
        return YAN_FS_NOT_FOUND;
    }
""",
        "new": """    if (source_name == destination_name) {
        return YAN_FS_EXISTS; /* mutant: a pointer-equal shortcut before the lookup */
    }
    uint32_t source_slot = 0;
    if (!find_entry(fs, source_name, source_length, &source_slot)) {
        return YAN_FS_NOT_FOUND;
    }
""",
    },
    {
        "name": "copy_borrows_source_extent",
        "file": "os/yanfs.c",
        "old": """    uint32_t destination_start = 0;
    if (block_count > 0u &&
        !allocate_extent(fs, block_count, &destination_start)) {
        return YAN_FS_NOSPACE;
    }
""",
        "new": """    uint32_t destination_start = start_block; /* mutant: borrow the source extent */
""",
    },
    {
        "name": "copy_skips_final_tail_zero",
        "file": "os/yanfs.c",
        "old": """        for (uint32_t j = chunk; j < YAN_FS_BLOCK_SIZE; ++j) {
            fs->scratch[j] = 0u;
        }
""",
        "new": """        for (uint32_t j = chunk; j < chunk; ++j) {
            fs->scratch[j] = 0u; /* mutant: the final tail is not cleared */
        }
""",
    },
    {
        "name": "copy_publishes_candidate_before_write",
        "file": "os/yanfs.c",
        "old": """    entry_set(fs->scratch, slot, (const uint8_t *)destination_name,
              destination_length, size_bytes, destination_start, block_count);
    return commit_metadata(fs);
""",
        "new": """    entry_set(fs->scratch, slot, (const uint8_t *)destination_name,
              destination_length, size_bytes, destination_start, block_count);
    for (uint32_t i = 0; i < YAN_FS_BLOCK_SIZE; ++i) {
        fs->metadata[i] = fs->scratch[i]; /* mutant: candidates published first */
    }
    return commit_metadata(fs);
""",
    },
    {
        "name": "copy_reads_neighbour_block",
        "file": "os/yanfs.c",
        "old": """        uint32_t chunk = remaining < YAN_FS_BLOCK_SIZE ? remaining
                                                       : YAN_FS_BLOCK_SIZE;
        YanFsIoResult io = fs->io.read_block(fs->io.context,
                                             start_block + block_index,
                                             fs->scratch);
""",
        "new": """        uint32_t chunk = remaining < YAN_FS_BLOCK_SIZE ? remaining
                                                       : YAN_FS_BLOCK_SIZE;
        YanFsIoResult io = fs->io.read_block(fs->io.context,
                                             start_block + block_index + 1u,
                                             fs->scratch);
""",
    },
    {
        "name": "copy_ignores_source_read_error",
        "file": "os/yanfs.c",
        "old": """        uint32_t chunk = remaining < YAN_FS_BLOCK_SIZE ? remaining
                                                       : YAN_FS_BLOCK_SIZE;
        YanFsIoResult io = fs->io.read_block(fs->io.context,
                                             start_block + block_index,
                                             fs->scratch);
        if (io != YAN_FS_IO_OK) {
            return fault_io(fs, io);
        }
""",
        "new": """        uint32_t chunk = remaining < YAN_FS_BLOCK_SIZE ? remaining
                                                       : YAN_FS_BLOCK_SIZE;
        YanFsIoResult io = fs->io.read_block(fs->io.context,
                                             start_block + block_index,
                                             fs->scratch);
        if (io != YAN_FS_IO_OK) {
            (void)io; /* mutant: a source read error is ignored */
        }
""",
    },
]

# Two real controls. The no-effect replacement compiles and must pass; the
# wrong-owner entry reuses the tail mutation but declares a test that passes.
NO_EFFECT = {
    "name": "no-effect-control",
    "file": "os/yanfs.c",
    "old": "#include <stddef.h>\n",
    "new": "#include <stddef.h> /* no-effect control */\n",
    "owner": "read_handles_all_lengths_and_tail_zero",
    "assertion": "no-effect-control",
}
WRONG_OWNER = {
    "name": "wrong-owner-control",
    "file": "os/yanfs.c",
    "old": "            fs->scratch[j] = j < chunk ? bytes[offset + j] : 0u;\n",
    "new": "            fs->scratch[j] = j < chunk ? bytes[offset + j] : 0xA5u;\n",
    "owner": "format_encodes_capacity_little_endian",
    "assertion": "wrong-owner-control",
}

FOOTER = re.compile(r"^\s*(\d+) Tests (\d+) Failures (\d+) Ignored\s*$")
RECORD = re.compile(
    r"^([^\s:]+):(\d+):([A-Za-z_][A-Za-z0-9_]*):(PASS|FAIL|IGNORE)\b(.*)$")
BREAKER = re.compile(r"^\s*-{3,}\s*$")
TERMINAL = re.compile(r"^\s*(OK|FAIL)\s*$")
HARNESS_HINTS = (b"sanitizer", b"runtime error:", b"harness-error", b"fixture abort")


class HarnessError(RuntimeError):
    """A condition that is not a clean detection verdict."""


def parse_unity(stdout):
    """Strict decode of one Unity suite's stdout.

    Returns (tests, failures, ignored, fail_names, fail_messages) only for a
    complete, self-consistent run. fail_messages maps a failing test name to the
    message Unity printed after "FAIL:" (the assertion detail), so a caller can
    require the *specific* assertion that the mutation is meant to trip instead
    of crediting any failure of that test. Anything else -- empty stdout, partial
    output, a missing or duplicated footer, a count mismatch -- returns None,
    which the caller treats as a harness error, never as a detection.
    """
    text = stdout.decode("utf-8", "replace")
    lines = [line.rstrip("\r") for line in text.split("\n")]
    footers = []
    records = []
    fail_messages = {}
    for index, line in enumerate(lines):
        footer = FOOTER.match(line)
        if footer is not None:
            footers.append((index, tuple(int(value) for value in footer.groups())))
            continue
        record = RECORD.match(line)
        if record is not None:
            name = record.group(3)
            status = record.group(4)
            message = record.group(5).strip()
            records.append((index, name, status))
            if status == "FAIL":
                fail_messages[name] = message
    names = [name for _index, name, _status in records]
    if len(set(names)) != len(names):
        # Duplicate records for the same test (two FAILs, or a PASS and a FAIL)
        # make the Unity output ambiguous: reject the whole run as harness, even
        # if a fake footer's counts would otherwise look consistent.
        return None
    if len(footers) != 1:
        return None
    footer_index, (tests, failures, ignored) = footers[0]
    if footer_index == 0 or BREAKER.match(lines[footer_index - 1]) is None:
        return None
    if footer_index + 1 >= len(lines):
        return None
    terminal = TERMINAL.match(lines[footer_index + 1])
    if terminal is None:
        return None
    if (terminal.group(1) == "OK") != (failures == 0):
        return None
    if any(index > footer_index for index, _name, _status in records):
        return None
    fail_names = [name for _index, name, status in records if status == "FAIL"]
    ignored_names = [name for _index, name, status in records if status == "IGNORE"]
    if tests < 1 or tests != len(records):
        return None
    if failures != len(fail_names) or ignored != len(ignored_names):
        return None
    return tests, failures, ignored, fail_names, fail_messages


def classify_run(returncode, stdout, stderr, owner, assertion=None):
    """Pure verdict for one suite's run.

    A detection needs a complete Unity run whose exit code equals the footer's
    failure count, whose failing tests include the declared owner, and whose
    owner FAIL message contains the required `assertion` marker (the specific
    assertion the mutation is meant to trip). A missing marker is a harness
    error, never a detection: an unrelated failure of the same owner must not be
    credited. Exit 0 with no output, a bare FAIL without a footer, a signal
    (negative or the shell's 128+ form), a timeout, a sanitizer/abort report, or
    any counter inconsistency is a harness error, never a detection.
    """
    text = stdout + b"\n" + stderr
    lowered = text.lower()
    if returncode is None:
        return "harness"
    if returncode < 0 or returncode >= 128:
        return "harness"
    if any(hint in lowered for hint in HARNESS_HINTS):
        return "harness"
    parsed = parse_unity(stdout)
    if parsed is None:
        return "harness"
    _tests, failures, _ignored, fail_names, fail_messages = parsed
    if returncode != failures:
        return "harness"
    if returncode == 0:
        if failures != 0 or fail_names:
            return "harness"
        return "pass"
    if not fail_names:
        return "harness"
    if not owner:
        return "owner-fail"
    if not assertion:
        # An actual mutant must supply its expected assertion marker; missing
        # metadata is never silently accepted as a detection.
        return "harness"
    if owner not in fail_names:
        return "wrong-owner"
    if assertion in fail_messages.get(owner, ""):
        return "owner-fail"
    return "wrong-assertion"


def apply_replacement(path, old, new):
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise HarnessError("%s: replacement matched %d times, expected exactly 1"
                           % (path.name, count))
    path.write_text(text.replace(old, new, 1))


def copy_tree(source, unity, destination):
    (destination / "os").mkdir(parents=True)
    (destination / "tests").mkdir()
    (destination / "unity").mkdir()
    for name in OS_FILES:
        shutil.copy2(source / "os" / name, destination / "os" / name)
    for name in TEST_FILES:
        shutil.copy2(source / "tests" / name, destination / "tests" / name)
    for item in sorted((unity / "src").iterdir()):
        if item.is_file():
            shutil.copy2(item, destination / "unity" / item.name)


def unity_object(tree, cc):
    """Unity is an external dependency, not code under test: build it once per
    tree without the project warning bar, exactly as its own CMake target does.
    The strict flags stay on the sources the mutation touches."""
    out_dir = tree / "bin"
    out_dir.mkdir(exist_ok=True)
    obj = out_dir / "unity.o"
    if obj.exists():
        return obj
    command = [cc, "-std=c17", "-I", str(tree / "unity"),
               "-c", str(tree / "unity/unity.c"), "-o", str(obj)]
    try:
        proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("unity build timeout") from error
    if proc.returncode != 0:
        raise HarnessError("unity build failed:\n%s%s"
                           % (proc.stdout.decode(errors="replace"),
                              proc.stderr.decode(errors="replace")))
    return obj


def compile_suite(tree, cc, suite, strict):
    flags = STRICT_FLAGS if strict else MUTANT_FLAGS
    unity = tree / "unity"
    out_dir = tree / "bin"
    out_dir.mkdir(exist_ok=True)
    obj = unity_object(tree, cc)
    if suite == "core":
        sources = [tree / "tests/test_yanfs.c", tree / "os/yanfs.c"]
        includes = ["-I", str(tree / "os"), "-I", str(unity)]
        target = out_dir / "test_yanfs"
    else:
        sources = [tree / "tests/test_yanfs_adapter.c"]
        includes = ["-I", str(unity)]
        target = out_dir / "test_yanfs_adapter"
    command = [cc] + flags + includes + [str(path) for path in sources] + \
              [str(obj), "-o", str(target)]
    try:
        proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("build timeout: %s" % suite) from error
    return proc, target, command


def format_build_report(entries):
    """The exact compile commands and their output, kept in every log so the
    warning bar used for a mutant is auditable."""
    chunks = []
    for suite, command, proc in entries:
        chunks.append("# build %s: %s" % (suite, " ".join(command)))
        chunks.append(proc.stdout.decode(errors="replace"))
        chunks.append(proc.stderr.decode(errors="replace"))
    return "\n".join(chunks).encode()


def build_both(tree, cc, strict):
    core_proc, core, core_command = compile_suite(tree, cc, "core", strict)
    adapter_proc, adapter, adapter_command = compile_suite(tree, cc, "adapter",
                                                           strict)
    report = format_build_report([("core", core_command, core_proc),
                                  ("adapter", adapter_command, adapter_proc)])
    return core_proc, core, adapter_proc, adapter, report


def run_binary(path):
    try:
        proc = subprocess.run([str(path)], capture_output=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, b"", b""
    return proc.returncode, proc.stdout, proc.stderr


def run_suites(bin_core, bin_adapter, owner, assertion=None):
    rc_core, out_core, err_core = run_binary(bin_core)
    rc_adapter, out_adapter, err_adapter = run_binary(bin_adapter)
    verdict_core = classify_run(rc_core, out_core, err_core, owner, assertion)
    verdict_adapter = classify_run(rc_adapter, out_adapter, err_adapter, owner,
                                   assertion)
    blob = out_core + err_core + out_adapter + err_adapter
    verdicts = (verdict_core, verdict_adapter)
    if "harness" in verdicts:
        return "harness", blob
    if "owner-fail" in verdicts:
        return "owner-fail", blob
    if "wrong-assertion" in verdicts:
        return "wrong-assertion", blob
    if "wrong-owner" in verdicts:
        return "wrong-owner", blob
    return "pass", blob


def unity_suite(records):
    """Build a complete, self-consistent Unity stdout for the mocked controls.

    A record is (name, status) or (name, status, message); the message is the
    assertion detail after "FAIL:".
    """
    failures = 0
    ignored = 0
    lines = []
    for index, record in enumerate(records, start=1):
        name = record[0]
        status = record[1]
        message = record[2] if len(record) > 2 else "message"
        if status == "FAIL":
            failures += 1
        elif status == "IGNORE":
            ignored += 1
        suffix = (": " + message) if status in ("FAIL", "IGNORE") else ""
        lines.append("tests/test_yanfs.c:%d:%s:%s%s" % (index, name, status, suffix))
    lines.append("-----------------------")
    lines.append("%d Tests %d Failures %d Ignored " % (len(records), failures,
                                                       ignored))
    lines.append("OK" if failures == 0 else "FAIL")
    return ("\n".join(lines) + "\n").encode()


def self_test_run_suites():
    """Permanent mocked controls for run_suites' precedence and the required
    assertion marker.

    A harness symptom on either suite must win over an owner failure on the
    other; the owner must fail on the required assertion, not merely on some
    assertion of the same test; an owner failure must win over a wrong-owner
    failure. This runs without a compiler, so yanfs_mutation_gate covers it in
    the default suite.
    """
    marker = "OWNER-ASSERTION-MARKER"
    pass_suite = (0, unity_suite([("a", "PASS"), ("b", "PASS")]), b"")
    owner_suite = (1, unity_suite([("owner", "FAIL", marker)]), b"")
    wrong_assertion_suite = (1, unity_suite([("owner", "FAIL", "other value")]), b"")
    other_suite = (1, unity_suite([("other", "FAIL", marker)]), b"")
    no_marker_suite = (1, unity_suite([("owner", "FAIL")]), b"")
    empty_suite = (0, b"", b"")
    timeout_suite = (None, b"", b"")
    signal_suite = (139, unity_suite([("owner", "FAIL", marker)]), b"")
    exit2_suite = (2, b"", b"")
    cases = [
        (pass_suite, pass_suite, marker, "pass"),
        (owner_suite, pass_suite, marker, "owner-fail"),
        (pass_suite, owner_suite, marker, "owner-fail"),
        (wrong_assertion_suite, pass_suite, marker, "wrong-assertion"),
        (other_suite, pass_suite, marker, "wrong-owner"),
        (other_suite, owner_suite, marker, "owner-fail"),
        (no_marker_suite, pass_suite, None, "harness"),
        (no_marker_suite, pass_suite, marker, "wrong-assertion"),
        (empty_suite, owner_suite, marker, "harness"),
        (owner_suite, empty_suite, marker, "harness"),
        (timeout_suite, owner_suite, marker, "harness"),
        (signal_suite, pass_suite, marker, "harness"),
        (exit2_suite, owner_suite, marker, "harness"),
    ]
    original = run_binary
    try:
        for index, (core, adapter, assertion, expected) in enumerate(cases):
            responses = {"test_yanfs": core, "test_yanfs_adapter": adapter}

            def stub(path, _responses=responses):
                return _responses[Path(path).name]

            globals()["run_binary"] = stub
            verdict, _blob = run_suites("/mock/test_yanfs",
                                        "/mock/test_yanfs_adapter", "owner",
                                        assertion)
            if verdict != expected:
                raise HarnessError(
                    "run_suites control %d: got %s, expected %s"
                    % (index, verdict, expected))
    finally:
        globals()["run_binary"] = original


def owner_exists(source, owner, mutation_file):
    test = "tests/test_yanfs_adapter.c" if "yanfs_block" in mutation_file \
        else "tests/test_yanfs.c"
    text = (source / test).read_text()
    return ("RUN_TEST(%s)" % owner) in text and \
           ("static void %s(void)" % owner) in text


def write_log(logdir, name, blob):
    (logdir / (name + ".log")).write_bytes(blob)


def owner_fail_line(blob, owner):
    needle = b":" + owner.encode() + b":FAIL:"
    for line in blob.splitlines():
        if needle in line:
            return line.decode(errors="replace")
    return "(no owner FAIL line recorded)"


def run_mutation(source, tree_root, cc, mutation, logdir):
    owner = mutation["owner"]
    assertion = mutation.get("assertion")
    if not assertion:
        # An actual mutant must declare the assertion it is meant to trip; never
        # fall back to "any failure of the owner".
        raise HarnessError("%s: no required assertion marker" % mutation["name"])
    tree = tree_root / ("mut-" + mutation["name"])
    shutil.copytree(tree_root / "baseline", tree,
                    ignore=shutil.ignore_patterns("bin"))
    apply_replacement(tree / mutation["file"], mutation["old"], mutation["new"])
    core_build, core_bin, adapter_build, adapter_bin, build_report = build_both(
        tree, cc, False)
    if core_build.returncode != 0 or adapter_build.returncode != 0:
        write_log(logdir, mutation["name"], build_report)
        raise HarnessError("%s: mutant build failed" % mutation["name"])
    verdict, blob = run_suites(core_bin, adapter_bin, owner, assertion)
    write_log(logdir, mutation["name"], build_report + b"\n" + blob)
    return verdict, blob


def main():
    parser = argparse.ArgumentParser(description="YanFS mutation gate")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--unity", required=True, type=Path)
    args = parser.parse_args()

    try:
        self_test_run_suites()
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        return 2
    print("PASS mocked run_suites precedence controls")

    if shutil.which(args.cc) is None:
        print("SKIP: external C compiler missing: %s" % args.cc)
        return 77
    unity = args.unity.resolve()
    if not (unity / "src/unity.c").is_file() or not (unity / "src/unity.h").is_file():
        print("SKIP: external Unity missing under %s/src" % unity)
        return 77

    source = args.source.resolve()
    missing = [str(source / "os" / name) for name in OS_FILES
               if not (source / "os" / name).is_file()]
    missing += [str(source / "tests" / name) for name in TEST_FILES
                if not (source / "tests" / name).is_file()]
    if missing:
        print("FAIL the source/tests under test are missing: %s" % ", ".join(missing),
              file=sys.stderr)
        return 1
    for name in OWNING_ASSERTIONS:
        match = next(item for item in MUTATIONS if item["name"] == name)
        if not owner_exists(source, OWNING_ASSERTIONS[name][0], match["file"]):
            print("FAIL owning test %s is not registered for %s"
                  % (OWNING_ASSERTIONS[name][0], name), file=sys.stderr)
            return 1

    args.work.mkdir(parents=True, exist_ok=True)
    logdir = args.work / "yanfs-mutation"
    logdir.mkdir(exist_ok=True)
    tree_root = Path(tempfile.mkdtemp(prefix="yanfs-mut-", dir=args.work))
    harness = []
    survivors = []
    try:
        copy_tree(source, unity, tree_root / "baseline")
        core_build, core_bin, adapter_build, adapter_bin, build_report = build_both(
            tree_root / "baseline", args.cc, True)
        if core_build.returncode != 0 or adapter_build.returncode != 0:
            write_log(logdir, "baseline", build_report)
            print("HARNESS-ERROR: strict baseline build failed", file=sys.stderr)
            return 2
        verdict, blob = run_suites(core_bin, adapter_bin, "")
        write_log(logdir, "baseline", build_report + b"\n" + blob)
        if verdict != "pass":
            print("HARNESS-ERROR: baseline is %s, not a clean pass" % verdict,
                  file=sys.stderr)
            return 2
        print("PASS baseline builds strict and both suites pass")

        for control, expected in ((NO_EFFECT, "pass"), (WRONG_OWNER, "wrong-owner")):
            entry = dict(control)
            entry["owner"] = control["owner"]
            entry["assertion"] = control["assertion"]
            outcome, _ = run_mutation(source, tree_root, args.cc, entry, logdir)
            if outcome != expected:
                print("HARNESS-ERROR: control %s was %s, expected %s"
                      % (control["name"], outcome, expected), file=sys.stderr)
                return 2
            print("PASS control %s: %s" % (control["name"], outcome))

        for mutation in MUTATIONS:
            mutation = dict(mutation)
            owner, assertion = OWNING_ASSERTIONS[mutation["name"]]
            mutation["owner"] = owner
            mutation["assertion"] = assertion
            try:
                outcome, blob = run_mutation(source, tree_root, args.cc, mutation,
                                             logdir)
            except HarnessError as error:
                harness.append((mutation["name"], str(error)))
                print("HARNESS-ERROR %s: %s" % (mutation["name"], error))
                continue
            if outcome == "owner-fail":
                print("PASS mutant %s: owner %s failed on %r -> %s"
                      % (mutation["name"], mutation["owner"], mutation["assertion"],
                         owner_fail_line(blob, mutation["owner"])))
            elif outcome == "harness":
                harness.append((mutation["name"], "run classified as harness"))
                print("HARNESS-ERROR %s: run classified as harness (never "
                      "detected)" % mutation["name"])
            elif outcome == "wrong-assertion":
                survivors.append((mutation["name"], "wrong assertion"))
                print("FAIL mutant %s: owner %s failed, but not on the required"
                      " assertion %r" % (mutation["name"], mutation["owner"],
                                         mutation["assertion"]))
            elif outcome == "wrong-owner":
                wrong = "a test other than %s failed" % mutation["owner"]
                survivors.append((mutation["name"], wrong))
                print("FAIL mutant %s: wrong owner (%s)" % (mutation["name"], wrong))
            else:
                survivors.append((mutation["name"], outcome))
                print("FAIL mutant %s: %s" % (mutation["name"], outcome))
    finally:
        shutil.rmtree(tree_root, ignore_errors=True)

    print("---")
    print("%d mutants detected, %d survivors/wrong-owner, %d harness errors; "
          "logs in %s" % (len(MUTATIONS) - len(survivors) - len(harness),
                          len(survivors), len(harness), logdir))
    if harness:
        return 2
    if survivors:
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        sys.exit(2)
