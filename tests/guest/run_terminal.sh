#!/usr/bin/env bash
# Builds and runs the UART terminal smoke test.
#
# The UART device is implemented and unit-tested, but until a Host backend is
# attached it has no other end: CONNECTED stays 0 and TXDATA is refused. This
# script is the executable check of the other end, and it needs both halves of
# the contract:
#
#   1. `yan_run --terminal` with a pipe on standard input: the Guest's three
#      bytes must land on standard output byte for byte, and nothing else may.
#   2. `yan_run --terminal` with a regular file on standard input: the Host must
#      not depend on a TTY to feed the receiver.
#   3. no `--terminal`: the image must still take the CONNECTED=0 path from
#      docs/specs/0015-uart-device.md, report it through `tohost` and let the
#      runner exit on its own. Every run is bounded by `timeout`, so a Guest
#      that waits forever for a missing terminal is reported as a failure
#      instead of hanging the suite.
#   4. the receive line reaches the CPU: the Guest enables the UART receive
#      interrupt, routes it through PLIC source 2 and waits for MEIP; the byte
#      fed on standard input must arrive as an interrupt and come back on
#      standard output. This case only passes while the executor samples the
#      device lines before every step -- without that the PLIC never sees the
#      line and the Guest reports check code 21.
#   5. the command-line contract: `-h` and `--help` put the usage on standard
#      output and exit 0, while an unknown option -- and no arguments at all --
#      put the same usage on standard error and exit 2. These are exactly the
#      behaviours a later refactor of the option parser can silently invert
#      (exit 2, or the wrong stream), so they are pinned here instead of being
#      checked by hand once.
#
# Exit codes: 0 all cases behaved, 1 a run or an assertion failed, 2 a usage
# error, 77 this machine lacks a dependency that lives outside the repository
# (the cross toolchain or timeout(1)). The command-line cases need no cross
# toolchain and run before the toolchain check: a missing compiler skips the
# Guest half with 77 but can never hide a command-line regression, which exits 1
# first. The sources under test, including the executor built from this tree,
# are not dependencies: a missing one exits 1.
set -u

gcc=""
run=""
work=""
source=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "run_terminal.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

# --gcc is optional on purpose: the command-line cases below only need the
# executor, and requiring the toolchain here would skip them on a build host
# without a RISC-V compiler.
if [ -z "$run" ] || [ -z "$work" ]; then
    echo "usage: run_terminal.sh --run YAN_RUN --work DIR [--gcc RISCV_GCC] [--source DIR]" >&2
    exit 2
fi

# Default to the checkout this script lives in, so a build tree anywhere can
# point at these sources without passing --source.
if [ -z "$source" ]; then
    source="$(cd "$(dirname "$0")/../.." && pwd)"
fi

mkdir -p "$work"
# The runs below must not be able to hang the suite, whether a Guest waits for a
# ready bit that never comes or the executor mis-handles an option.
# timeout(1) is a host tool outside the repository: without it the check cannot
# bound its runs, so that is a dependency and stays a skip. The executor is not:
# it is built from this tree, so a missing one is a build that did not happen.
command -v timeout > /dev/null 2>&1 || { echo "SKIP: missing timeout(1)"; exit 77; }
if [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    exit 1
fi

# ------------------------------------------------------------ command line
# One invocation, recorded: status, standard output and standard error are all
# part of the contract, so all three are captured and reported on failure.
cli_status=0
cli_out_bytes=0
cli_err_bytes=0
invoke() {
    timeout 60 "$@" > "$work/cli.out" 2> "$work/cli.err"
    cli_status=$?
    cli_out_bytes=$(wc -c < "$work/cli.out")
    cli_err_bytes=$(wc -c < "$work/cli.err")
}

fail_cli() {
    echo "FAIL $1" >&2
    echo "    exit=$cli_status stdout=${cli_out_bytes}B stderr=${cli_err_bytes}B" >&2
    for log in "$work/cli.out" "$work/cli.err"; do
        [ -s "$log" ] || continue
        echo "--- $log:" >&2
        sed 's/^/    /' "$log" >&2
    done
    exit 1
}

# A usage message, not just any output: both spellings must carry the switch
# this milestone added, so a help text that forgets --terminal fails here.
check_usage_text() {
    grep -q '^usage: ' "$1" || fail_cli "$2 did not print a usage message"
    grep -q -- '--terminal' "$1" || fail_cli "$2 usage does not document --terminal"
}

invoke "$run" -h
[ "$cli_status" -eq 0 ] || fail_cli "-h must exit 0"
[ "$cli_out_bytes" -gt 0 ] || fail_cli "-h must print the usage on standard output"
[ "$cli_err_bytes" -eq 0 ] || fail_cli "-h must not write to standard error"
check_usage_text "$work/cli.out" "-h"
cp "$work/cli.out" "$work/help_short.txt"
echo "PASS -h prints the usage on standard output and exits 0"

invoke "$run" --help
[ "$cli_status" -eq 0 ] || fail_cli "--help must exit 0"
[ "$cli_out_bytes" -gt 0 ] || fail_cli "--help must print the usage on standard output"
[ "$cli_err_bytes" -eq 0 ] || fail_cli "--help must not write to standard error"
check_usage_text "$work/cli.out" "--help"
# Both spellings must come from the same text, or the two drift apart.
cmp -s "$work/help_short.txt" "$work/cli.out" ||
    fail_cli "-h and --help printed different text"
echo "PASS --help prints the same usage on standard output and exits 0"

invoke "$run" --definitely-not-an-option
[ "$cli_status" -eq 2 ] || fail_cli "an unknown option must exit 2"
[ "$cli_out_bytes" -eq 0 ] || fail_cli "an unknown option must not write to standard output"
check_usage_text "$work/cli.err" "an unknown option"
echo "PASS an unknown option prints the usage on standard error and exits 2"

invoke "$run"
[ "$cli_status" -eq 2 ] || fail_cli "no arguments must exit 2"
[ "$cli_out_bytes" -eq 0 ] || fail_cli "no arguments must not write to standard output"
check_usage_text "$work/cli.err" "no arguments"
echo "PASS no arguments prints the usage on standard error and exits 2"

# ------------------------------------------------------------ Guest cases
# From here on a cross toolchain is required; without one the Guest half is
# skipped, and the command-line assertions above have already run.
if [ -z "$gcc" ] || [ ! -e "$gcc" ]; then
    echo "SKIP the Guest cases: no RISC-V compiler was given"
    exit 77
fi

guest_dir="$source/tests/guest"
# Everything below is the thing under test: its sources live in this repository,
# so a missing one is a hard failure. 77 is reserved for a dependency that lives
# outside the repository (the cross toolchain, checked above): CTest records 77
# as a skip, and a skip is not a pass.
missing=0
for required in "$guest_dir/terminal_check.c" \
                "$guest_dir/start.S" "$guest_dir/mtrap_entry.S" \
                "$guest_dir/mtrap.c" "$guest_dir/guest_lib.c" \
                "$guest_dir/guest_devices.h" "$guest_dir/link.ld"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

# The step budget covers the whole program with room to spare; a Guest that
# never terminates is reported as exit 4 by the runner, not as a pass.
steps=200000

elf="$work/terminal_check.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -T "$guest_dir/link.ld" \
        "$guest_dir/start.S" "$guest_dir/terminal_check.c" \
        "$guest_dir/guest_lib.c" \
        -Wl,--build-id=none -o "$elf" > "$work/build.log" 2>&1; then
    echo "FAIL the Guest terminal check does not build:"
    sed 's/^/    /' "$work/build.log"
    exit 1
fi

# The expected output is a file, not a shell string: command substitution would
# strip the trailing newline and weaken the byte-for-byte comparison.
printf 'OK\n' > "$work/expected.bin"

show_bytes() {
    echo "--- $1 (od -c):" >&2
    od -c "$1" | sed 's/^/    /' >&2
}

fail() {
    echo "FAIL $1" >&2
    shift
    for log in "$@"; do
        [ -s "$log" ] || continue
        echo "--- $log:" >&2
        sed 's/^/    /' "$log" >&2
    done
    exit 1
}

check_output() {
    if ! cmp -s "$1" "$work/expected.bin"; then
        echo "FAIL $2 wrote something other than 'OK\\n'" >&2
        show_bytes "$1"
        fail "$2 stdout differed" "$3"
    fi
}

# 1. A pipe on standard input. The bytes fed here are never echoed; the point is
#    that a pipe is a working input stream and that output is byte-exact.
printf 'piped input is not echoed by this program\n' |
    timeout 60 "$run" --image "$elf" --max-steps "$steps" --terminal \
        > "$work/pipe.out" 2> "$work/pipe.err"
status=$?
[ "$status" -eq 0 ] ||
    fail "case 1 (--terminal, stdin pipe) exited with $status (124 means timeout)" \
         "$work/pipe.err"
check_output "$work/pipe.out" "case 1 (--terminal, stdin pipe)" "$work/pipe.err"
echo "PASS with --terminal a stdin pipe produced exactly 'OK\\n'"

# 2. A regular file on standard input: the same run without a TTY on stdin.
printf 'file input\n' > "$work/input.bin"
timeout 60 "$run" --image "$elf" --max-steps "$steps" --terminal \
    < "$work/input.bin" > "$work/file.out" 2> "$work/file.err"
status=$?
[ "$status" -eq 0 ] ||
    fail "case 2 (--terminal, stdin file) exited with $status (124 means timeout)" \
         "$work/file.err"
check_output "$work/file.out" "case 2 (--terminal, stdin file)" "$work/file.err"
echo "PASS with --terminal a stdin file produced exactly 'OK\\n'"

# 3. No --terminal: no backend, so the Guest must take the CONNECTED=0 path,
#    report check code 10 and terminate by itself. Exit 4 would mean it never
#    reached `tohost`, 5 would mean it faulted on an unmapped window.
printf 'ignored\n' | timeout 60 "$run" --image "$elf" --max-steps "$steps" \
    > "$work/headless.out" 2> "$work/headless.err"
status=$?
[ "$status" -eq 6 ] ||
    fail "case 3 (no --terminal) exited with $status, expected 6 from the Guest failure path" \
         "$work/headless.err"
[ -s "$work/headless.out" ] &&
    { show_bytes "$work/headless.out"; fail "case 3 wrote output with no terminal attached" "$work/headless.err"; }
grep -q "failure code 10" "$work/headless.err" ||
    fail "case 3 did not report check code 10 (the CONNECTED=0 path)" "$work/headless.err"
echo "PASS without --terminal the Guest reported the missing terminal and exited"

# 4. The receive line must reach the CPU through the platform's interrupt
#    wiring. The image is built from the same source in its interrupt mode: the
#    Guest installs a handler, routes PLIC source 2 to MEIP and waits. The byte
#    fed here has to arrive as an interrupt and be echoed back, so the check
#    covers stdin -> push_rx -> receive line -> PLIC -> MEIP -> RXDATA -> stdout.
#    Without device-line sampling the PLIC stays quiet and the Guest reports
#    check code 21 (exit 6) instead.
meip_elf="$work/terminal_meip.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -DTERMINAL_CHECK_MEIP=1 -T "$guest_dir/link.ld" \
        "$guest_dir/mtrap_entry.S" "$guest_dir/mtrap.c" \
        "$guest_dir/terminal_check.c" "$guest_dir/guest_lib.c" \
        -Wl,--build-id=none -o "$meip_elf" > "$work/build_meip.log" 2>&1; then
    echo "FAIL the Guest interrupt check does not build:"
    sed 's/^/    /' "$work/build_meip.log"
    exit 1
fi
printf 'Q' | timeout 60 "$run" --image "$meip_elf" --max-steps 2000000 \
    --terminal > "$work/meip.out" 2> "$work/meip.err"
status=$?
[ "$status" -eq 0 ] ||
    fail "case 4 (--terminal, UART receive interrupt) exited with $status (124 means timeout; 6 with check code 21 means the PLIC never saw the line)" \
         "$work/meip.err"
printf 'Q\n' > "$work/expected_meip.bin"
if ! cmp -s "$work/meip.out" "$work/expected_meip.bin"; then
    echo "FAIL case 4 echoed something other than 'Q\\n'" >&2
    show_bytes "$work/meip.out"
    fail "case 4 stdout differed" "$work/meip.err"
fi
echo "PASS with --terminal the Host byte arrived as a UART receive interrupt"

echo "PASS the Host terminal backend carries Guest output, the headless path terminates and the receive line reaches the CPU"
exit 0
