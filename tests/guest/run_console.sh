#!/usr/bin/env bash
# Builds and runs the Guest console self-check: docs/specs/0017-console-and-os-layout.md
# section B (B1 - B8) against os/console.c, plus the section C mutation check.
#
# The Guest images are built here and run through the real CPU, Bus and UART
# device in src/. Two drives can carry them:
#
#   scripted  a runner built into the work directory from a generated source.
#             It links the Host libraries tools/yan_run.c links and attaches a
#             *scripted* UART backend. Two of the B-group states have no spelling
#             on a stdin/stdout terminal and need it: CONNECTED=1 while
#             TX_READY=0 (B3), and a terminal that disappears while getline waits
#             (B7).
#
#   yan-run   tools/yan_run with --terminal, the durable Host entry point for a
#             Guest console (docs/specs/0015 IMPLE PLAN step 6). Used when the
#             option exists; every scenario whose terminal behaviour standard
#             input and output can express is rerun through it and its captured
#             bytes compared again.
#
# Neither drive is allowed to weaken an assertion. A scenario that cannot run is
# reported as PENDING with the reason, never as a pass, and --strict turns any
# pending scenario into exit 1 so a caller can refuse to call it green.
#
# Exit codes: 0 every check that ran passed, 1 a check, scenario or mutation
# failed - including a requested drive that could not be built and a --strict
# run with something still pending - 2 usage, 77 this machine has no cross
# toolchain. 77 means "a dependency that lives outside the repository" and
# nothing else: the console driver and its Guest check are the thing under test,
# so a missing one fails instead of skipping.
#
# CTest: register it the way tests/guest/run_trap_env.sh is registered
# (--source/--gcc/--run/--work, SKIP_RETURN_CODE 77, TIMEOUT 300). This script
# takes about 8 s, and a second test running "--mutation --drive scripted" takes
# about 35 s and fails the suite when a planted defect survives.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
source="$(cd "$script_dir/../.." && pwd)"
gcc=""
run=""
work=""
cc="${CC:-cc}"
console_c=""
drive="auto"
strict=0
mutation=0

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --console) console_c="$2"; shift 2 ;;
        --drive) drive="$2"; shift 2 ;;
        --strict) strict=1; shift ;;
        --mutation) mutation=1; shift ;;
        *) echo "run_console.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$work" ]; then
    echo "usage: run_console.sh --gcc RISCV_GCC --work DIR [--run YAN_RUN]" \
         "[--source DIR] [--cc HOST_CC] [--console FILE]" \
         "[--drive auto|scripted|yan-run] [--strict] [--mutation]" >&2
    exit 2
fi
case "$drive" in
    auto|scripted|yan-run) ;;
    *) echo "run_console.sh: --drive must be auto, scripted or yan-run" >&2; exit 2 ;;
esac

guest_dir="$source/tests/guest"
os_dir="$source/os"
[ -n "$console_c" ] || console_c="$os_dir/console.c"

# 77 means "a dependency this machine does not have", and nothing else: the
# cross toolchain is the only thing outside the repository this check needs.
# The console driver, its Guest check and the shared Guest sources are the thing
# under test, so a missing one is a hard failure. Deleting an implementation
# must never be a way to a green suite: CTest records 77 as a skip, and a skip
# is not a pass.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
missing=0
for required in "$console_c" "$os_dir/console.h" "$os_dir/platform.h" \
                "$guest_dir/console_check.c" "$guest_dir/mtrap_entry.S" \
                "$guest_dir/mtrap.c" "$guest_dir/guest_lib.c" \
                "$guest_dir/link.ld"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
# An executor that was named but is not there is a build that did not happen,
# not a missing dependency. An unnamed executor is not this script's business:
# the scripted drive does not need it, and the yan-run drive says so below.
if [ -n "$run" ] && [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    exit 1
fi
[ "$missing" -eq 0 ] || exit 1

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

mkdir -p "$work/images" "$work/capture" "$work/mutants"
rm -f "$work"/images/*.elf 2>/dev/null

# ------------------------------------------------------------------ scenarios
#
# feeds are hex byte streams the backend hands to RXDATA one byte at a time;
# captures are the exact bytes that must have been delivered to TXDATA. Both are
# byte lists rather than text so escape sequences stay unambiguous. A scenario
# with an empty capture asserts that nothing at all was delivered.
#
# steps bounds every run. A driver that waits for a byte or a ready bit that
# never comes ends with "no termination" instead of a verdict, which is the
# step-limit proof B1 asks for: the limit is far above the few thousand
# instructions a passing run needs and far below forever.
#
# The three echo-refused scenarios use --refuse-tx-after N to reach the state
# getline's echo cannot be written in: CONNECTED = 1 while TX_READY = 0 from the
# Nth delivered byte on. B3 uses the same option for putc. Their feeds carry a
# line ending after the refusal point, so a driver that ignores the refusal
# completes the line and fails a check instead of polling forever.
names=(headless output refused edit boundary crlf disconnect high-byte \
       echo-refused echo-refused-backspace echo-refused-line-end)
macros=(HEADLESS OUTPUT REFUSED EDIT BOUNDARY CRLF DISCONNECT HIGH_BYTE \
        ECHO_REFUSED ECHO_REFUSED_BACKSPACE ECHO_REFUSED_LINE_END)
feeds=("" "" "" "0808616208630a" "61626364650a" "610d0a620a" "6162" "800a" \
       "610a" "6162080a" "610d")
captures=("" "6869" "6869" "6162082008630d0a" "616263640d0a" "610d0a620d0a" "6162" "800d0a" \
          "" "6162" "61")
steps=(40000 40000 40000 60000 60000 60000 60000 40000 40000 40000 40000)
# The disconnect case leaves the backend in place until well after the Guest has
# read and echoed its last byte, so the call under test is one that is waiting
# for input, not one caught in the middle of a character. A driver that is busy
# elsewhere when the terminal leaves may therefore have delivered fewer bytes
# than the script feeds; for that one case the capture is checked as a prefix,
# since B7 asserts the return value, not the echo of an arbitrary cut. The
# echo-refused scenarios cover the "interrupted while echoing" state on purpose,
# with a refusal the driver can see rather than a cut it cannot.
capture_rules=(exact exact exact exact exact exact prefix exact exact exact exact)
expected_case=("no terminal: the device must deliver nothing" \
               'puts("hi")' \
               'two bytes accepted, then TX_READY=0' \
               'two backspaces on an empty line, "ab", backspace, "c", LF' \
               '"abcde\n" into a capacity-5 buffer' \
               '"a\r\nb\n"' \
               '"ab" and then the terminal leaves' \
               '0x80 then LF' \
               'the echo of the first character is refused' \
               'the echo of the backspace is refused after "ab"' \
               'the echo of the CR line ending is refused')

scenario_extra_options() {
    case "$1" in
        headless) printf '%s' "--no-terminal" ;;
        refused) printf '%s' "--refuse-tx-after 2" ;;
        disconnect) printf '%s' "--disconnect-when-drained --disconnect-after-drain 2000" ;;
        echo-refused) printf '%s' "--refuse-tx-after 0" ;;
        echo-refused-backspace) printf '%s' "--refuse-tx-after 2" ;;
        echo-refused-line-end) printf '%s' "--refuse-tx-after 1" ;;
        *) printf '%s' "" ;;
    esac
}

# ------------------------------------------------------------------ image build
build_image() { # $1 = scenario macro, $2 = console source, $3 = output elf
    "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -I "$guest_dir" -I "$os_dir" \
        -DYAN_CONSOLE_SCENARIO="YAN_CONSOLE_SCENARIO_$1" \
        -T "$guest_dir/link.ld" \
        "$guest_dir/mtrap_entry.S" "$guest_dir/mtrap.c" \
        "$guest_dir/guest_lib.c" "$guest_dir/console_check.c" "$2" \
        -Wl,--build-id=none -o "$3"
}

# ------------------------------------------------------------------ scripted drive
#
# The generated runner is not part of the repository's tools: it exists so the B
# group can be run and mutated today, and it goes away once tools/yan_run.c can
# attach a scripted terminal of its own.
write_drive_source() {
    cat > "$work/console_drive.c" <<'CONSOLE_DRIVE_EOF'
/* Scripted-backend Guest drive for the console self-check.
 *
 * Generated into the work directory by tests/guest/run_console.sh; kept out of
 * the repository because the durable Host entry point for a Guest console is
 * `yan_run --terminal`. It links the same Host libraries (RAM, Bus, CPU, CLINT,
 * PLIC, UART) and adds the three things the console B group has to control:
 * whether a backend is attached, whether it accepts a byte, and which bytes
 * arrive.
 *
 * Exit codes match tools/yan_run.c: 0 tohost PASS, 2 usage, 4 no termination,
 * 5 Host error, 6 the Guest reported a failure code.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_file.h"
#include "yan/cpu.h"
#include "yan/image.h"
#include "yan/interrupt.h"

enum {
    EXIT_PASS = 0,
    EXIT_USAGE = 2,
    EXIT_NO_TERMINATION = 4,
    EXIT_HOST_ERROR = 5,
    EXIT_GUEST_FAILURE = 6
};

#define FEED_CAPACITY 256

typedef struct {
    unsigned delivered;
    long refuse_after; /* negative: the backend never refuses */
    FILE *capture;
} Backend;

static bool backend_tx_ready(void *context)
{
    const Backend *backend = context;
    return backend->refuse_after < 0 ||
           backend->delivered < (unsigned)backend->refuse_after;
}

static void backend_tx_write(void *context, uint8_t byte)
{
    Backend *backend = context;
    ++backend->delivered;
    if (backend->capture != NULL) {
        fputc(byte, backend->capture);
    }
}

static int number(const char *text, uint64_t *value)
{
    char *end = NULL;
    *value = strtoull(text, &end, 0);
    return end != text && *end == '\0';
}

static unsigned hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
    return 16;
}

static int parse_hex(const char *text, uint8_t *out, size_t *length)
{
    const size_t digits = strlen(text);
    if (digits % 2 != 0 || digits / 2 > FEED_CAPACITY) {
        return 0;
    }
    for (size_t at = 0; at < digits; at += 2) {
        const unsigned high = hex_digit(text[at]);
        const unsigned low = hex_digit(text[at + 1]);
        if (high > 15 || low > 15) {
            return 0;
        }
        out[at / 2] = (uint8_t)((high << 4) | low);
    }
    *length = digits / 2;
    return 1;
}

int main(int argc, char **argv)
{
    const char *image_path = NULL;
    const char *capture_path = NULL;
    const char *feed_hex = NULL;
    uint64_t base = UINT32_C(0x80000000);
    uint64_t ram_size = 16U * 1024U * 1024U;
    uint64_t max_steps = 1000000;
    long refuse_after = -1;
    long disconnect_after_drain = 0;
    int no_terminal = 0;
    int disconnect_when_drained = 0;

    for (int i = 1; i < argc; ++i) {
        uint64_t value = 0;
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image_path = argv[++i];
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture_path = argv[++i];
        else if (strcmp(argv[i], "--feed-hex") == 0 && i + 1 < argc) feed_hex = argv[++i];
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            base = value;
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            ram_size = value;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &max_steps)) return EXIT_USAGE;
        } else if (strcmp(argv[i], "--refuse-tx-after") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            refuse_after = (long)value;
        } else if (strcmp(argv[i], "--disconnect-after-drain") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            disconnect_after_drain = (long)value;
        } else if (strcmp(argv[i], "--no-terminal") == 0) {
            no_terminal = 1;
        } else if (strcmp(argv[i], "--disconnect-when-drained") == 0) {
            disconnect_when_drained = 1;
        } else {
            return EXIT_USAGE;
        }
    }
    if (image_path == NULL || ram_size == 0) {
        return EXIT_USAGE;
    }
    if (no_terminal && (feed_hex != NULL || disconnect_when_drained)) {
        /* Nothing to feed and nothing to detach without a backend. */
        return EXIT_USAGE;
    }
    if (disconnect_when_drained && feed_hex == NULL) {
        /* Detaching before the Guest has read anything would test the wrong
         * thing: B7 is about a terminal that leaves mid-poll. */
        return EXIT_USAGE;
    }

    YanRam ram = {0};
    YanBus bus = {0};
    YanCpu cpu = {0};
    YanClint clint = {0};
    YanPlic plic = {0};
    YanUart uart = {0};
    YanImageInfo info = {0};
    uint8_t *image = NULL;
    size_t image_size = 0;
    uint8_t feed[FEED_CAPACITY] = {0};
    size_t feed_length = 0;
    size_t cursor = 0;
    long drain_hold = 0;
    int attached = 0;
    int result = EXIT_HOST_ERROR;
    int stopped = 0;
    uint32_t tohost = 0;
    int tohost_known = 0;

    Backend backend = {.delivered = 0, .refuse_after = refuse_after, .capture = NULL};
    const YanUartTerminal terminal = {
        .context = &backend, .tx_ready = backend_tx_ready, .tx_write = backend_tx_write};

    if (feed_hex != NULL && !parse_hex(feed_hex, feed, &feed_length)) {
        fprintf(stderr, "console_drive: --feed-hex is not an even hex byte list\n");
        return EXIT_USAGE;
    }
    if (capture_path != NULL) {
        backend.capture = fopen(capture_path, "wb");
        if (backend.capture == NULL) {
            fprintf(stderr, "console_drive: cannot write '%s'\n", capture_path);
            return EXIT_HOST_ERROR;
        }
    }
    if (yan_ram_init(&ram, (uint32_t)ram_size) != YAN_OK ||
        yan_bus_init(&bus, &ram, (uint32_t)base) != YAN_OK) {
        goto done;
    }
    /* Mapped exactly as yan_run maps them, plus the UART this drive exists for. */
    yan_clint_reset(&clint);
    yan_plic_reset(&plic);
    yan_uart_reset(&uart);
    bus.clint = &clint;
    bus.plic = &plic;
    bus.uart = &uart;
    if (!no_terminal) {
        if (yan_uart_set_terminal(&uart, &terminal) != YAN_OK) {
            goto done;
        }
        attached = 1;
    }
    image = yan_host_read_file(image_path, &image_size);
    if (image == NULL) {
        fprintf(stderr, "console_drive: cannot read '%s'\n", image_path);
        goto done;
    }
    if (yan_image_load_elf(&ram, (uint32_t)base, image, image_size, &info) != YAN_OK) {
        fprintf(stderr, "console_drive: '%s' is not a loadable RV32 ELF image\n",
                image_path);
        goto done;
    }
    if (yan_cpu_reset(&cpu, info.entry) != YAN_OK) {
        goto done;
    }
    if (yan_image_find_symbol(image, image_size, "tohost", &tohost) == YAN_OK) {
        tohost_known = 1;
    }

    for (uint64_t step = 0; step < max_steps; ++step) {
        /* The Host end of the device, driven between instructions: hand over the
         * next scripted byte while the single-byte receive buffer is free, and
         * detach once the script has been read by the Guest. Detaching earlier
         * would throw the unread byte away with the terminal. The hold keeps the
         * backend in place for a while after the last byte has been taken, so the
         * Guest is waiting for input - which is the state B7 is about - instead
         * of being caught in the middle of echoing it. */
        if (attached && cursor < feed_length && !uart.rx_available) {
            if (yan_uart_push_rx(&uart, feed[cursor]) == YAN_OK) {
                ++cursor;
            }
        }
        if (attached && disconnect_when_drained && cursor == feed_length &&
            !uart.rx_available) {
            if (drain_hold < disconnect_after_drain) {
                ++drain_hold;
            } else {
                (void)yan_uart_set_terminal(&uart, NULL);
                attached = 0;
            }
        } else {
            drain_hold = 0;
        }
        yan_clint_tick(&clint, 1);
        uint32_t instruction = 0;
        if (yan_cpu_fetch(&cpu, &bus, &instruction).status != YAN_OK) {
            fprintf(stderr, "console_drive: cannot fetch at pc = %08" PRIx32 "\n",
                    cpu.pc);
            goto done;
        }
        (void)yan_cpu_step(&cpu, &bus);
        if (tohost_known) {
            uint32_t value = 0;
            if (yan_bus_read(&bus, tohost, 4, &value).status == YAN_OK && value != 0) {
                if (value != 1) {
                    fprintf(stderr, "console_drive: the Guest reported failure code"
                            " %" PRIu32 " after %" PRIu64 " instructions\n",
                            value, step + 1);
                } else {
                    fprintf(stderr, "console_drive: tohost PASS after %" PRIu64
                            " instructions, %u byte(s) delivered\n",
                            step + 1, backend.delivered);
                }
                result = value == 1 ? EXIT_PASS : EXIT_GUEST_FAILURE;
                stopped = 1;
                break;
            }
        }
    }
    if (!stopped && result == EXIT_HOST_ERROR) {
        fprintf(stderr, "console_drive: stopped after %" PRIu64
                " steps without reaching tohost\n", max_steps);
        result = EXIT_NO_TERMINATION;
    }
done:
    if (backend.capture != NULL) {
        fclose(backend.capture);
    }
    free(image);
    yan_ram_destroy(&ram);
    return result;
}
CONSOLE_DRIVE_EOF
}

build_scripted_drive() {
    write_drive_source
    "$cc" -O1 -std=c17 -I "$source/include" -I "$source/tools" \
        -o "$work/console_drive" "$work/console_drive.c" \
        "$source/tools/host_file.c" "$source"/src/*.c > "$work/drive-build.log" 2>&1
}

# ------------------------------------------------------------------ helpers
hex_to_bin() { # $1 = hex string, $2 = output file
    local hex="$1" out="$2" at
    : > "$out"
    for ((at = 0; at < ${#hex}; at += 2)); do
        printf "\\x${hex:at:2}" >> "$out"
    done
}

reported_code() { # $1 = log file; prints the failure code the runner named
    sed -n 's/.*reported failure code \([0-9][0-9]*\) .*/\1/p' "$1" | head -n 1
}

# $1 = scenario index, $2 = capture file. True when what the driver delivered is
# exactly the expected stream, or - for the one scenario whose cut point the
# driver does not control - a prefix of it.
capture_matches() {
    local index="$1" capture="$2"
    local expected="$work/capture/expected-${names[index]}.bin"
    hex_to_bin "${captures[index]}" "$expected"
    if cmp -s "$expected" "$capture"; then
        return 0
    fi
    if [ "${capture_rules[index]}" = "prefix" ]; then
        local size
        size="$(stat -c %s "$capture")"
        head -c "$size" "$expected" | cmp -s - "$capture"
        return $?
    fi
    return 1
}

# The Guest's own panic handler ends the run with 0xbad0 | cause
# (tests/guest/mtrap.h), which is not one of the console check numbers.
is_trap_code() { # $1 = code
    case "${1:-}" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -ge $((0xbad0)) ]
}

# $1 = drive label, $2 = scenario index, $3 = exit status, $4 = capture file,
# $5 = log file. Prints the verdict; returns nonzero when the scenario failed.
report_case() {
    local label="$1" index="$2" status="$3" capture="$4" log="$5"
    local name="${names[index]}" code
    code="$(reported_code "$log")"
    if [ "$status" -ne 0 ]; then
        case "$status" in
            4) echo "FAIL [$label] ${name}: no termination within ${steps[index]} steps (the driver waited)";;
            6) if is_trap_code "$code"; then
                   echo "FAIL [$label] ${name}: the Guest trapped (code $code)"
               else
                   echo "FAIL [$label] ${name}: check $code failed"
               fi ;;
            5) echo "FAIL [$label] ${name}: the Host drive failed";;
            *) echo "FAIL [$label] ${name}: the drive exited $status";;
        esac
        sed 's/^/    /' "$log" | tail -n 3
        return 1
    fi
    local expected="$work/capture/expected-${name}.bin"
    hex_to_bin "${captures[index]}" "$expected"
    if ! capture_matches "$index" "$capture"; then
        echo "FAIL [$label] ${name}: captured bytes differ (${expected_case[index]})"
        echo "    expected: $(od -An -tx1 "$expected" | tr -s ' ')"
        echo "    observed: $(od -An -tx1 "$capture" | tr -s ' ')"
        return 1
    fi
    if [ -n "${captures[index]}" ]; then
        echo "PASS [$label] ${name}: captured $(od -An -tx1 "$expected" | tr -s ' ')"
    else
        echo "PASS [$label] ${name}: nothing delivered, verdict reached inside the step limit"
    fi
    return 0
}

# $1 = scenario index, $2 = elf. Runs the scripted drive; prints the verdict.
run_scripted_case() {
    local index="$1" elf="$2" name="${names[index]}"
    local capture="$work/capture/scripted-${name}.bin"
    local log="$work/capture/scripted-${name}.log"
    read -r -a extra <<< "$(scenario_extra_options "$name")"
    read -r -a feed_option <<< "$( [ -n "${feeds[index]}" ] && printf '%s' "--feed-hex ${feeds[index]}" )"
    "$work/console_drive" --image "$elf" --capture "$capture" \
        --max-steps "${steps[index]}" "${extra[@]}" "${feed_option[@]}" \
        > "$log" 2>&1
    local status=$?
    report_case scripted "$index" "$status" "$capture" "$log"
}

# $1 = scenario index, $2 = elf. Runs tools/yan_run --terminal: standard output
# is the capture and standard input carries the scenario's bytes. Only scenarios
# that need no scripted refusal or detach can use this drive.
run_yan_run_case() {
    local index="$1" elf="$2" name="${names[index]}"
    local capture="$work/capture/yan-run-${name}.bin"
    local log="$work/capture/yan-run-${name}.log"
    local feed="$work/capture/yan-run-${name}.feed"
    local status
    hex_to_bin "${feeds[index]}" "$feed"
    if [ "$index" -eq 0 ]; then
        # No --terminal at all: the Guest must run headless and deliver nothing.
        "$run" --image "$elf" --max-steps "${steps[index]}" > "$capture" 2> "$log"
        status=$?
    else
        # The trailing sleep holds the write end open, so a backend that reads
        # "the other end went away" from EOF cannot mistake the end of the script
        # for a terminal that left while the Guest was still reading. A scenario
        # whose input ends in a line ending does not depend on it either way.
        { cat "$feed"; sleep 1; } | "$run" --image "$elf" --terminal \
            --max-steps "${steps[index]}" > "$capture" 2> "$log"
        status=$?
    fi
    report_case yan-run "$index" "$status" "$capture" "$log"
}

# ------------------------------------------------------------------ main flow
echo "console: gcc=$gcc drive=$drive console=$console_c work=$work"

unrunnable=0
failed=0

# ---- scripted drive ---------------------------------------------------------
scripted=0
if [ "$drive" != "yan-run" ]; then
    if build_scripted_drive; then
        scripted=1
    else
        echo "PENDING scripted drive: cannot build it from $source/src:"
        tail -n 5 "$work/drive-build.log" | sed 's/^/    /'
        unrunnable=$((unrunnable + 1))
    fi
fi
if [ "$drive" = "scripted" ] && [ "$scripted" -eq 0 ]; then
    # The scripted drive is built from this repository's own sources, so a
    # requested drive that cannot be built is a failure, not a missing
    # dependency: 77 here would let a broken src/ or a broken driver source skip
    # the test and still show up as green in the CTest summary.
    echo "FAIL the requested scripted drive is unavailable"
    exit 1
fi

if [ "$scripted" -eq 1 ]; then
    for index in "${!names[@]}"; do
        elf="$work/images/${names[index]}.elf"
        if ! build_image "${macros[index]}" "$console_c" "$elf" \
                > "$work/images/${names[index]}.log" 2>&1; then
            echo "FAIL the Guest image for ${names[index]} does not build:"
            tail -n 5 "$work/images/${names[index]}.log" | sed 's/^/    /'
            failed=1
            continue
        fi
        run_scripted_case "$index" "$elf" || failed=1
    done
fi

# ---- yan_run integration drive ----------------------------------------------
terminal_supported=0
if [ -n "$run" ] && [ -x "$run" ] && "$run" --help 2>&1 | grep -q -- '--terminal'; then
    terminal_supported=1
fi

if [ "$drive" = "scripted" ]; then
    echo "note: --drive scripted was given, so no integration run happened"
elif [ "$terminal_supported" -eq 1 ]; then
    # Every scenario whose terminal behaviour a stdin/stdout backend can express
    # is rerun here: B1 and B8 headless, B2 output, B4 editing, B5 the buffer
    # boundary, B6 CRLF. B3 needs the device to refuse a byte while connected and
    # B7 needs the terminal to leave mid-poll; neither has a stdin spelling, so
    # they stay on the scripted drive and are reported as pending here.
    for index in 0 1 3 4 5 7; do
        elf="$work/images/${names[index]}.elf"
        if [ ! -e "$elf" ]; then
            if ! build_image "${macros[index]}" "$console_c" "$elf" \
                    > "$work/images/${names[index]}.log" 2>&1; then
                echo "FAIL the Guest image for ${names[index]} does not build"
                failed=1
                continue
            fi
        fi
        run_yan_run_case "$index" "$elf" || failed=1
    done
    echo "PENDING yan-run drive: B3 (CONNECTED=1 with TX_READY=0) and B7 (the terminal"
    echo "        leaves mid-poll) have no stdin spelling; they ran on the scripted"
    echo "        drive, and moving them here needs a tool-side scripted backend."
    unrunnable=$((unrunnable + 2))
elif [ -z "$run" ]; then
    echo "PENDING yan-run drive: --run was not given, so B1, B2, B4, B5, B6 and B8 did"
    echo "        not run through the durable Host entry point either"
    unrunnable=$((unrunnable + 6))
elif [ ! -x "$run" ]; then
    echo "PENDING yan-run drive: '$run' is not executable, so B1, B2, B4, B5, B6 and"
    echo "        B8 did not run through the durable Host entry point either"
    unrunnable=$((unrunnable + 6))
else
    echo "PENDING yan-run drive: '$run' has no --terminal, so B1, B2, B4, B5, B6 and B8"
    echo "        did not run through the durable Host entry point, and B3/B7 did not"
    echo "        run there either"
    unrunnable=$((unrunnable + 8))
fi

# ---- mutation check ---------------------------------------------------------
if [ "$mutation" -eq 1 ]; then
    if [ "$scripted" -eq 0 ]; then
        # Same reason as above: without the scripted drive the planted defects
        # cannot be judged at all, and "the check could not run" must not be
        # reported in a way CTest turns into a pass or a quiet skip.
        echo "FAIL mutation check: the scripted drive is unavailable, so no defect can be judged"
        exit 1
    fi
    echo "mutation: planting the section C defects in copies under $work/mutants"
    console_before="$(md5sum < "$console_c")"
    mutant_names=(headless-polls putc-writes-without-ready-check \
                  putc-ignores-refusal-result puts-ignores-refusal \
                  backspace-without-empty-check boundary-writes-past-capacity \
                  crlf-not-swallowed capacity-zero-not-rejected \
                  echo-not-written echo-refusal-ignored)
    survivors=0
    detected=0
    for mutant in "${mutant_names[@]}"; do
        copy="$work/mutants/$mutant.c"
        rm -f "$copy"
        cp -a "$console_c" "$copy"
        before="$(md5sum < "$copy")"
        case "$mutant" in
            headless-polls)
                sed -i 's#^        if (!yan_os_uart_connected()) {#        if (0) {#' "$copy" ;;
            putc-writes-without-ready-check)
                # Writes TXDATA without consulting TX_READY at all: the device
                # refuses the byte, so nothing is delivered, but the driver says
                # it succeeded.
                sed -i 's#    if (yan_os_uart_put((uint8_t)c) != 0) {#    YAN_OS_MMIO_WRITE32(YAN_OS_UART_BASE + YAN_OS_UART_TXDATA, (uint32_t)(uint8_t)c);\n    if (0) {#' "$copy" ;;
            putc-ignores-refusal-result)
                # Consults the device but drops its answer, which is the softer
                # version of the same defect: the byte is refused, the caller is
                # told it was written.
                sed -i 's#    if (yan_os_uart_put((uint8_t)c) != 0) {#    (void)yan_os_uart_put((uint8_t)c);\n    if (0) {#' "$copy" ;;
            puts-ignores-refusal)
                sed -i 's#^        if (result != YAN_OS_OK) {#        if (0) {#' "$copy" ;;
            backspace-without-empty-check)
                sed -i 's#^            if (stored > 0) {#            if (1) {#' "$copy" ;;
            boundary-writes-past-capacity)
                sed -i 's#^        if (stored + 1 >= capacity) {#        if (0) {#' "$copy" ;;
            crlf-not-swallowed)
                sed -i 's#swallow_lf = byte == .\\r.;#swallow_lf = 0;#' "$copy" ;;
            capacity-zero-not-rejected)
                sed -i 's#    if (buffer == NULL || capacity == 0 || length == NULL) {#    if (buffer == NULL) {#' "$copy" ;;
            echo-not-written)
                # The echo call is dropped entirely: the bytes never reach the
                # terminal, so the capture assertions bite, and a refusal cannot
                # be noticed because nothing is written at all. One sed per echo
                # call site, because the character, the backspace and the line
                # ending are separate code.
                sed -i 's#        if (yan_os_console_putc((char)byte) != YAN_OS_OK) {#        if (0) {#' "$copy"
                sed -i 's#                if (yan_os_console_puts("\\b \\b") != YAN_OS_OK) {#                if (0) {#' "$copy"
                sed -i 's#            if (yan_os_console_puts("\\r\\n") != YAN_OS_OK) {#            if (0) {#' "$copy" ;;
            echo-refusal-ignored)
                # The echo is still written, its result is dropped: the terminal
                # never showed the character, the erase or the line ending, yet
                # getline completes the line. This isolates the refused-echo
                # branch itself, so only the scenarios that refuse an echo can
                # catch it.
                sed -i 's#        if (yan_os_console_putc((char)byte) != YAN_OS_OK) {#        (void)yan_os_console_putc((char)byte);\n        if (0) {#' "$copy"
                sed -i 's#                if (yan_os_console_puts("\\b \\b") != YAN_OS_OK) {#                (void)yan_os_console_puts("\\b \\b");\n                if (0) {#' "$copy"
                sed -i 's#            if (yan_os_console_puts("\\r\\n") != YAN_OS_OK) {#            (void)yan_os_console_puts("\\r\\n");\n            if (0) {#' "$copy" ;;
        esac
        after="$(md5sum < "$copy")"
        if [ "$before" = "$after" ]; then
            echo "FAIL mutation $mutant was not applied (pattern mismatch)"
            survivors=$((survivors + 1))
            continue
        fi
        caught_by=""
        build_failures=0
        for index in "${!names[@]}"; do
            elf="$work/images/mutant-$mutant-${names[index]}.elf"
            if ! build_image "${macros[index]}" "$copy" "$elf" \
                    > "$work/images/mutant-$mutant-${names[index]}.log" 2>&1; then
                build_failures=$((build_failures + 1))
                continue
            fi
            capture="$work/capture/mutant-$mutant-${names[index]}.bin"
            log="$work/capture/mutant-$mutant-${names[index]}.log"
            read -r -a extra <<< "$(scenario_extra_options "${names[index]}")"
            read -r -a feed_option <<< "$( [ -n "${feeds[index]}" ] && printf '%s' "--feed-hex ${feeds[index]}" )"
            "$work/console_drive" --image "$elf" --capture "$capture" \
                --max-steps "${steps[index]}" "${extra[@]}" "${feed_option[@]}" \
                > "$log" 2>&1
            status=$?
            code="$(reported_code "$log")"
            if [ "$status" -ne 0 ]; then
                if is_trap_code "$code"; then
                    caught_by="$caught_by ${names[index]}(trap $code)"
                elif [ "$status" -eq 4 ]; then
                    caught_by="$caught_by ${names[index]}(no termination)"
                else
                    caught_by="$caught_by ${names[index]}(exit $status, check ${code:-?})"
                fi
                continue
            fi
            expected="$work/capture/expected-${names[index]}.bin"
            hex_to_bin "${captures[index]}" "$expected"
            if ! capture_matches "$index" "$capture"; then
                caught_by="$caught_by ${names[index]}(capture)"
            fi
        done
        [ "$build_failures" -eq 0 ] || echo "    note: $build_failures scenario image(s) did not build for this mutant"
        if [ -n "$caught_by" ]; then
            echo "PASS mutation $mutant detected by:$caught_by"
            detected=$((detected + 1))
        else
            echo "FAIL mutation $mutant survived every scenario"
            survivors=$((survivors + 1))
        fi
    done
    console_after="$(md5sum < "$console_c")"
    if [ "$console_before" != "$console_after" ]; then
        echo "FAIL the mutation check modified $console_c"
        failed=1
    else
        echo "mutation: every defect was planted in a copy; $console_c is unchanged (md5 $console_after)"
    fi
    echo "mutation: $detected detected, $survivors survived"
    [ "$survivors" -eq 0 ] || failed=1
fi

# ---- verdict ----------------------------------------------------------------
if [ "$failed" -ne 0 ]; then
    echo "FAIL the console B group reported at least one failure"
    exit 1
fi
if [ "$unrunnable" -ne 0 ]; then
    echo "PENDING $unrunnable check(s)/driver(s) could not run (see above)"
    if [ "$strict" -eq 1 ]; then
        # --strict refuses to call a partial run green. That is a failure, not a
        # missing dependency: 77 here would turn "the evidence did not run" back
        # into a skip, which is exactly what strict is meant to refuse.
        echo "FAIL --strict was given and something is still pending"
        exit 1
    fi
fi
echo "PASS every console check that could run passed"
exit 0
