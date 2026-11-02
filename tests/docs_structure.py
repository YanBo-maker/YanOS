#!/usr/bin/env python3
"""Issue177 continuous invariants: 0034 R6-AC6.1 and R9-AC9.6 (row count).

  AC6.1: `wc -c docs/STATUS.md` <= 16384.
  AC9.6: data rows in docs/specs/README.md == one-level spec files + 1 (0002 row).

Data rows are linked four-digit IDs plus the bare 0002 placeholder. They are
counted, never resolved: no link targets, no uniqueness, no status column, no
recursion outside docs/specs. Exit 0 both hold, 1 a limit or count is violated,
2 a local input is missing or unusable.
"""
import argparse
import re
import sys
from pathlib import Path

STATUS_LIMIT = 16384
# Anchored first cell: `| [0001](0001-x.md) |` or the bare `| 0002 |` placeholder.
# [0-9] rather than \d, because the decided rows carry ASCII digits.
DATA_ROW = re.compile(r"^\|\s*(?:\[[0-9]{4}\]\([^|\r\n]*\)|0002)\s*\|")


def show(value):
    """ASCII rendering, safe under a cp1252 console and in collected logs."""
    return ascii(str(value))


def error(message):
    print(":ERROR: 0034_docs_structure: " + message, flush=True)
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1],
                        help="repository root (default: this script's repository)")
    root = parser.parse_args().source
    status = root / "docs" / "STATUS.md"
    specs_dir = root / "docs" / "specs"
    index = specs_dir / "README.md"
    try:
        # Bytes, not characters: STATUS.md is Chinese, three bytes per character.
        status_size = len(status.read_bytes())
        # One level only, so build/ and vendor copies of the docs stay out.
        specs = [entry for entry in specs_dir.iterdir()
                 if entry.is_file() and entry.suffix == ".md"
                 and entry.name != index.name]
        rows = index.read_bytes().decode("utf-8")
    except FileNotFoundError as exc:
        return error("missing local input: %s" % show(exc.filename))
    except UnicodeDecodeError as exc:
        return error("%s is not valid UTF-8: %s" % (show(index.name), show(exc)))
    except OSError as exc:
        return error("unusable local input: %s" % show(exc))
    if not specs or not rows.strip():
        return error("empty local input: %s holds %d spec files"
                     % (show(specs_dir), len(specs)))

    data_rows = sum(1 for line in rows.splitlines() if DATA_ROW.match(line))
    expected_rows = len(specs) + 1
    failed = False

    if status_size <= STATUS_LIMIT:
        print("0034 R6 AC6.1 PASS: docs/STATUS.md is %d bytes, limit %d"
              % (status_size, STATUS_LIMIT), flush=True)
    else:
        print(":FAIL: 0034_R6_AC6_1_status_bytes: docs/STATUS.md is %d bytes,"
              " limit %d" % (status_size, STATUS_LIMIT), flush=True)
        failed = True

    if data_rows == expected_rows:
        print("0034 R9 AC9.6 PASS: docs/specs/README.md has %d data rows,"
              " %d spec files + 1" % (data_rows, len(specs)), flush=True)
    else:
        print(":FAIL: 0034_R9_AC9_6_spec_index_rows: docs/specs/README.md has"
              " %d data rows, expected %d (%d spec files + 1)"
              % (data_rows, expected_rows, len(specs)), flush=True)
        failed = True

    if failed:
        return 1
    print("PASS: 0034_docs_structure: both invariants hold"
          " (count only, no link or status checks)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
