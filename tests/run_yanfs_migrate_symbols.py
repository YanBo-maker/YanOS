#!/usr/bin/env python3
"""Check the declared/defined/imported symbol mapping for migration fault tests.

``yan_migrate`` is the production Linux CLI.  Its fault twin declares a GNU ld
``--wrap=<name>`` for every production I/O symbol it replaces.  The link options
are the source of truth for the declared set; no separate list is maintained
here.  The fault binary supplies the ``__wrap_<name>`` definitions, and the
normal CLI supplies its dynamic undefined imports.  A pass means all three sets
agree for every declared wrapper.

This is declared-symbol coverage only; it is not all-I/O or
implementation-independent coverage.  Unrelated dynamic imports in the normal
CLI are expected and do not fail the check.  A redirected call shape (for
example ``readdir`` becoming ``readdir64`` under ``_FILE_OFFSET_BITS=64``) is
outside the current fault-wrapper/import mapping; the named mismatch reports it,
but the check does not remap it to the unsuffixed name.

A symbol appearing in the linked normal CLI does not prove that every source
call site is intercepted: direct syscalls, dlsym, inlining, and alternate call
shapes can escape.  The injection-marker tests and the independent data oracle
remain the behavioral evidence.  Registered wrappers are checked only for
declared-symbol existence and mapping.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

WRAP_LINK_OPTION = re.compile(r"(?:-Wl,)?--wrap=(?P<name>[A-Za-z_][A-Za-z0-9_]*)$")
DOUBLE_VERSION = re.compile(r"^(?P<name>.+)@@(?P<version>[A-Za-z0-9_.]+)$")
SINGLE_VERSION = re.compile(r"^(?P<name>.+)@(?P<version>[A-Za-z0-9_.]+)$")


class InputError(Exception):
    """Invalid input or a failed external tool; maps to exit code 2."""


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        print(f"ERROR: {message}", file=sys.stderr)
        raise SystemExit(2)


def normalize_argv(argv: list[str]) -> list[str]:
    """Keep a link-options value that starts with ``-Wl,`` visible to argparse."""
    normalized: list[str] = []
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument == "--link-options" and index + 1 < len(argv):
            normalized.append(f"{argument}={argv[index + 1]}")
            index += 2
        else:
            normalized.append(argument)
            index += 1
    return normalized


def link_option_tokens(args: argparse.Namespace, leftovers: list[str]) -> list[str]:
    values = [args.link_options, *leftovers]
    tokens: list[str] = []
    for value in values:
        tokens.extend(token for token in re.split(r"[;\s]+", value) if token)
    return tokens


def declared_wrappers(tokens: list[str]) -> set[str]:
    declared: set[str] = set()
    for token in tokens:
        match = WRAP_LINK_OPTION.fullmatch(token)
        if match is not None:
            name = match.group("name")
            if name in declared:
                raise InputError(f"duplicate --wrap declaration for '{name}'")
            declared.add(name)
        elif "--wrap=" in token:
            raise InputError(f"unparseable wrapper link option: {token!r}")
    return declared


def strip_version(raw: str) -> tuple[str, str | None]:
    """Return the symbol name and an optional malformed-version diagnostic."""
    if "@" not in raw:
        return raw, None
    for pattern in (DOUBLE_VERSION, SINGLE_VERSION):
        match = pattern.fullmatch(raw)
        if match is not None:
            return match.group("name"), None
    return raw, f"malformed ELF version suffix kept for {raw!r}"


def parse_nm(output: str, label: str) -> tuple[set[str], list[str]]:
    symbols: set[str] = set()
    warnings: list[str] = []
    for line_number, line in enumerate(output.splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) < 2:
            warnings.append(f"{label}: line {line_number} has no symbol: {line!r}")
            continue
        name, warning = strip_version(fields[-1])
        symbols.add(name)
        if warning is not None:
            warnings.append(f"{label}: line {line_number}: {warning}")
    return symbols, warnings


def run_nm(nm: Path, arguments: list[str], label: str) -> set[str]:
    command = [str(nm), *arguments]
    try:
        process = subprocess.run(command, text=True, capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise InputError(f"{label}: cannot run nm ({' '.join(command)}): {error}") from error
    if process.returncode != 0:
        detail = process.stderr.strip() or process.stdout.strip() or "no diagnostic"
        raise InputError(
            f"{label}: nm exited {process.returncode} ({' '.join(command)}): {detail}")
    if not process.stdout.strip():
        raise InputError(f"{label}: nm produced empty output ({' '.join(command)})")
    symbols, warnings = parse_nm(process.stdout, label)
    for warning in warnings:
        print(f"WARNING: {warning}", file=sys.stderr)
    return symbols


def require_nonempty(value: set[str], description: str) -> None:
    if not value:
        raise InputError(f"no {description} found")


def inspect(args: argparse.Namespace, leftovers: list[str]) -> int:
    for label, path in (("nm", args.nm), ("--migrate", args.migrate),
                        ("--fault", args.fault)):
        if not Path(path).is_file():
            raise InputError(f"{label} is not a file: {path}")

    tokens = link_option_tokens(args, leftovers)
    declared = declared_wrappers(tokens)
    imported = run_nm(Path(args.nm), ["-D", "--undefined-only", args.migrate],
                      "normal CLI imports")
    fault_symbols = run_nm(Path(args.nm), ["--defined-only", args.fault],
                           "fault definitions")
    defined = {name[7:] for name in fault_symbols if name.startswith("__wrap_")}

    require_nonempty(declared, "declared wrappers (link options)")
    require_nonempty(defined, "fault __wrap_ definitions")
    require_nonempty(imported, "normal CLI dynamic imports")

    mismatches = False
    for name in sorted(declared - defined):
        print(f"MISMATCH missing-fault-definition: declared wrapper '{name}' "
              f"has no '__wrap_{name}' in the fault binary")
        mismatches = True
    for name in sorted(defined - declared):
        print(f"MISMATCH unregistered-wrapper: fault defines '__wrap_{name}' "
              "but the link options do not declare it")
        mismatches = True
    for name in sorted(declared - imported):
        if f"{name}64" in imported:
            print(f"MISMATCH missing-production-call: declared wrapper '{name}' "
                  f"is not imported; candidate imports '{name}64' "
                  "(the current fault-wrapper/import mapping does not cover "
                  "this call shape; _FILE_OFFSET_BITS=64/LTO may be valid "
                  "production behavior, but is not covered or remapped here)")
        else:
            print(f"MISMATCH missing-production-call: declared wrapper '{name}' "
                  "is not imported by the normal CLI")
        mismatches = True
    if mismatches:
        return 1

    covered = sorted(declared & imported)
    print("PASS yanfs_migrate_symbols")
    print(f"declared wrappers ({len(declared)}): {', '.join(sorted(declared))}")
    print(f"defined wrappers: {len(defined)}")
    print(f"normal dynamic imports: {len(imported)}")
    print(f"covered: {', '.join(covered)}")
    print(f"ignored unrelated imports: {len(imported - declared)}")
    return 0


def main(argv: list[str]) -> int:
    parser = Parser(description="Check yan_migrate fault wrapper symbol coverage")
    parser.add_argument("--nm", required=True, help="nm executable to run explicitly")
    parser.add_argument("--migrate", required=True, help="normal yan_migrate CLI")
    parser.add_argument("--fault", required=True, help="yanfs_migrate_fault binary")
    parser.add_argument("--link-options", required=True,
                        help="actual LINK_OPTIONS of yanfs_migrate_fault")
    arguments, leftovers = parser.parse_known_args(normalize_argv(argv))
    try:
        return inspect(arguments, leftovers)
    except InputError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
