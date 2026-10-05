#!/usr/bin/env bash
# Mutation check for the block request protocol: docs/specs/0018-block-protocol.md,
# Guest side os/block.c and Host side tools/host_block.c.
#
# Why this exists: tests/guest/run_block.sh proves the protocol works, not that
# its checks would notice if it stopped working. This script plants one
# deliberate defect at a time in a private copy of one source file, rebuilds and
# reruns the self-check, and requires every defect to be caught. A survivor is a
# hole in the checks, not a curiosity.
#
# Detection criterion (deliberately strict, and stricter than "the suite was not
# green"): a planted defect counts as DETECTED only when the suite report says
# an assertion failed - "ASSERT-FAIL:" - *and* the log carries at least one
# ":FAIL:" line. Both halves are required on purpose: the marker says a check
# failed, and the double test means a future regression in the report script
# cannot turn some other failure into a detection. Everything else is classified
# separately and counts as NOT detected:
#
#   SURVIVED       the suite passed with the defect in place
#   INCONCLUSIVE   the run stopped without a verdict (trap, crash, sanitizer
#                  report, fixture abort, step limit, timeout)
#   BUILD-FAIL     the mutated source does not compile
#   APPLY-FAIL     the mutation could not be planted at all
#   TIMEOUT        the run outlived --timeout seconds
#
# The negative controls at the bottom of the table are what keeps that criterion
# honest. Each one is a change that must NOT be counted as a detection: a crash,
# a purely cosmetic diagnostic on standard error, a fixture that cannot build
# itself, a Guest trap after its checks already passed, and a mutation that is
# provably equivalent to the original code. If the criterion ever degrades - a
# crash read as a detection, a trap read as a failed check - these controls fail
# and say so.
#
# The equivalence control is the archived proof of a claim about the Host pump:
# removing *only* the "fewer than 16 bytes are not interpreted" guard is
# unkillable, because the frame-length check right below it refuses the same
# partial frames. Reproduction, for the record:
#
#   cp tools/host_block.c /tmp/e1.c
#   python3 -c 'p="/tmp/e1.c";s=open(p).read();open(p,"w").write(s.replace(
#       "        if (available < YAN_HOST_BLOCK_HEADER_SIZE) {",
#       "        if (available < 1U) {"))'
#   bash tests/guest/run_block.sh --source . --work /tmp/e1-work \
#       --gcc riscv64-linux-gnu-gcc --drive unit --host-block /tmp/e1.c
#   -> 278 check(s), 0 failed, exit 0
#   bash tests/guest/run_block.sh --source . --work /tmp/e1-work2 \
#       --gcc riscv64-linux-gnu-gcc --drive guest --host-block /tmp/e1.c
#   -> 19 check group(s) ran, 0 driver(s) unavailable, exit 0
#
# which is why the planted defect that *is* observable removes both guards
# (host-acts-on-a-partial-frame). When the second guard disappears the control
# starts being detected, and this table has to be revisited.
#
# The unmutated suite runs first: "the mutant was caught" is worthless if the
# suite was failing on its own.
#
# Layout: every copy lives in $work/run.<pid>/, so two runs sharing a work root
# cannot tread on each other, and the originals are never written - their md5
# sums are taken before the first mutation and compared again after the last.
#
# Exit codes: 0 every planted defect was caught by an assertion and every
# negative control behaved, 1 a defect was not caught (or a control was), 2
# usage, 77 a dependency is missing (no cross toolchain).
#
# Usage: tests/guest/run_block_mutation.sh --source DIR --gcc RISCV_GCC \
#            --run YAN_RUN --work DIR [--cc HOST_CC] [--timeout SECONDS] \
#            [--only NAME] [--keep]
set -u

source=""
work=""
gcc=""
run=""
cc="${CC:-cc}"
only=""
keep=0
timeout_s="${YAN_BLOCK_MUTATION_TIMEOUT:-300}"

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --only) only="$2"; shift 2 ;;
        --timeout) timeout_s="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        *) echo "run_block_mutation.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$source" ] || [ -z "$work" ] || [ -z "$gcc" ]; then
    echo "usage: run_block_mutation.sh --source DIR --gcc RISCV_GCC --run YAN_RUN" \
         "--work DIR [--cc HOST_CC] [--timeout SECONDS] [--only NAME] [--keep]" >&2
    exit 2
fi
case "$timeout_s" in ''|*[!0-9]*) echo "run_block_mutation.sh: --timeout takes seconds" >&2; exit 2 ;; esac

host_block="$source/tools/host_block.c"
guest_block="$source/os/block.c"
check_c="$source/tests/guest/block_check.c"
unit_c="$source/tests/guest/block_unit.c"
drive_c="$source/tests/guest/block_drive.c"
runner="$source/tests/guest/run_block.sh"

# 77 is for a dependency this machine does not have, and the cross toolchain is
# the only one. The sources below are the thing under test: a missing one is a
# hard failure, never a skip, or deleting an implementation would make this
# check green.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
missing=0
for required in "$host_block" "$guest_block" "$source/os/block.h" \
                "$source/os/platform.h" "$source/tools/host_block.h" "$check_c" \
                "$unit_c" "$drive_c" "$runner"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1
if [ -n "$run" ] && [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    exit 1
fi

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

run_dir="$work/run.$$"
mkdir -p "$run_dir/logs" "$run_dir/copy"
echo "mutation: scratch space $run_dir"

# The originals, before anything is planted.
host_before="$(md5sum < "$host_block")"
guest_before="$(md5sum < "$guest_block")"

# ------------------------------------------------------------- the mutations
#
# Every mutation is one or more literal old -> new replacements, each of which
# must occur exactly the expected number of times; the helper exits nonzero when
# one does not, so a refactor that moves a site is reported as an APPLY-FAIL
# instead of quietly mutating nothing. Production code is never reshaped to fit
# this file: when a defect's code shape disappears, the mutation is retired.
cat > "$run_dir/apply.py" <<'APPLY_EOF'
import sys

MUTATIONS = {
    # ---- Host: tools/host_block.c -----------------------------------------
    "host-acts-on-a-partial-frame": [
        # 0018 rules 1 and 4 together: the pump interprets whatever has arrived
        # and consumes it as if it were the whole frame. Removing only the first
        # guard would change nothing - the frame-length check below refuses the
        # same partial frames, which is what the equivalent-guard control proves
        # - so the defect is acting before either is satisfied.
        ("        if (available < YAN_HOST_BLOCK_HEADER_SIZE) {",
         "        if (available < 1U) {", 1),
        ("        if (available < frame_length) {\n"
         "            break;\n"
         "        }",
         "        if (available < frame_length) {\n"
         "            frame_length = available;\n"
         "        }", 1),
    ],
    "host-consumes-the-header-only": [
        # 0018 rule 5, read as "consume the header": the payload of a write stays
        # in the ring, so the next header is parsed from the middle of the frame
        # before it.
        ("        if (yan_transport_host_consume(transport, frame_length) != YAN_OK) {",
         "        if (yan_transport_host_consume(transport, YAN_HOST_BLOCK_HEADER_SIZE) != YAN_OK) {", 1),
    ],
    "host-keeps-a-rejected-frame": [
        # 0018 rule 5, read as "consume only what you accept". A frame that fails
        # validation is answered but left in the stream.
        ("        const uint32_t frame_at = transport->g2h_tail;\n"
         "        if (yan_transport_host_consume(transport, frame_length) != YAN_OK) {",
         "        const uint32_t frame_at = transport->g2h_tail;\n"
         "        if (status == YAN_HOST_BLOCK_STATUS_OK &&\n"
         "            yan_transport_host_consume(transport, frame_length) != YAN_OK) {", 1),
    ],
    "host-count-times-block-wraps": [
        # The frame length is computed in 32 bits, so a count whose product with
        # the block size wraps describes a frame that is not the one in the ring.
        ("    const uint64_t total =\n"
         "        (uint64_t)YAN_HOST_BLOCK_HEADER_SIZE +\n"
         "        (uint64_t)header->count * YAN_HOST_BLOCK_BLOCK_SIZE;\n"
         "    if (total > (uint64_t)(ring_size - 1U)) {\n"
         "        return 0;\n"
         "    }\n"
         "    *length = (uint32_t)total;",
         "    const uint32_t total =\n"
         "        YAN_HOST_BLOCK_HEADER_SIZE + header->count * YAN_HOST_BLOCK_BLOCK_SIZE;\n"
         "    if (total > ring_size - 1U) {\n"
         "        return 0;\n"
         "    }\n"
         "    *length = total;", 1),
    ],
    "host-publishes-a-partial-reply": [
        # 0018 "完成通知": the line is asserted for a frame the guest cannot read
        # to its end. Invisible while replies always fit - which is why the check
        # that catches it is the one that makes a reply not fit - and fatal as
        # soon as one does not: the guest is told there is data and then reads a
        # frame that never completes.
        ("        if (reply_length > yan_transport_host_writable(transport)) {\n"
         "            break;\n"
         "        }",
         "        const uint32_t room = yan_transport_host_writable(transport);\n"
         "        if (room == 0) {\n"
         "            break;\n"
         "        }", 1),
        ("        if (yan_transport_host_publish(transport, reply_length) != YAN_OK) {",
         "        if (yan_transport_host_publish(transport, reply_length < room ? reply_length : room) !=\n"
         "            YAN_OK) {", 1),
    ],
    "host-reports-blocks-on-a-failure": [
        # 0018: a response with a non-zero status claims no completed blocks.
        ("                        status == YAN_HOST_BLOCK_STATUS_OK ? header.count : 0);",
         "                        header.count);", 1),
    ],
    "host-does-not-echo-the-tag": [
        ("    bytes[2] = (uint8_t)tag;", "    bytes[2] = 0;", 1),
    ],
    "host-range-check-off-by-one": [
        # The last block of the device is refused as out of range.
        ("        } else if ((uint64_t)header.lba + header.count > block->capacity_blocks) {",
         "        } else if ((uint64_t)header.lba + header.count >= block->capacity_blocks) {", 1),
    ],
    "host-ring-load-no-mask": [
        # The request header is read without the modulo the ring needs, so a
        # header that crosses the wrap point is read from outside the ring. Only
        # the guest drive can witness this: its --request-offset row is the one
        # that puts a request header across the wrap, and the unit drive never
        # rotates the request ring. Found by review after the gate was shown to
        # be blind to a whole class of Host defects.
        ("    const uint8_t *source = view->data + ring;\n"
         "    for (uint32_t i = 0; i < count; ++i) {\n"
         "        out[i] = source[(index + i) & view->mask];\n"
         "    }",
         "    const uint8_t *source = view->data + ring;\n"
         "    for (uint32_t i = 0; i < count; ++i) {\n"
         "        out[i] = source[index + i];\n"
         "    }", 1),
    ],
    "host-consumes-the-fault-flag-early": [
        # The injected fault is spent by an attempt instead of by an answer, so a
        # response ring that is momentarily full swallows it (tools/host_block.h
        # promises it to "the next request that touches storage"). Found by
        # review; pinned by check_fault_waits_for_its_answer in block_unit.c.
        ("        if (faulted) {\n"
         "            status = YAN_HOST_BLOCK_STATUS_DEVICE;\n"
         "        }",
         "        if (faulted) {\n"
         "            status = YAN_HOST_BLOCK_STATUS_DEVICE;\n"
         "            block->fail_next = false;\n"
         "        }", 1),
    ],

    # ---- Guest: os/block.c -------------------------------------------------
    "guest-delivers-a-partial-header": [
        # 0018 rule 4: half a frame must not reach the caller, even though the
        # bytes are already in the ring and simply say the wrong thing.
        ("    /* Rule 4: nothing is consumed until the whole frame is there. */\n"
         "    if (available < total) {",
         "    /* Rule 4: nothing is consumed until the whole frame is there. */\n"
         "    if (available >= YAN_OS_BLOCK_HEADER_SIZE) {\n"
         "        response->op = peeked[0];\n"
         "        response->status = peeked[1];\n"
         "        response->tag = load_le16(peeked + 2);\n"
         "        response->lba = load_le32(peeked + 4);\n"
         "        response->count = load_le32(peeked + 8);\n"
         "    }\n"
         "    if (available < total) {", 1),
    ],
    "guest-consumes-the-header-only": [
        # The payload is delivered but left in the ring, so the next frame is
        # read from inside this one.
        ("    if (payload != 0 &&\n"
         "        yan_os_transport_h2g_pop(data, (uint32_t)payload) != 0) {\n"
         "        return YAN_OS_BLOCK_INVALID;\n"
         "    }",
         "    if (payload != 0) {\n"
         "        for (uint32_t i = 0; i < (uint32_t)payload; ++i) {\n"
         "            data[i] = peek_response(YAN_OS_BLOCK_HEADER_SIZE + i);\n"
         "        }\n"
         "    }", 1),
    ],
    "guest-drops-without-consuming": [
        # 0018 rule 5. A frame that is dropped is reported and left behind, so
        # every later response is read from the middle of it.
        ("        return drop_response(YAN_OS_BLOCK_HEADER_SIZE) == 0 ? YAN_OS_BLOCK_MALFORMED\n"
         "                                                            : YAN_OS_BLOCK_INVALID;",
         "        return YAN_OS_BLOCK_MALFORMED;", 1),
        ("        return drop_response((uint32_t)total) == 0 ? YAN_OS_BLOCK_MALFORMED\n"
         "                                                   : YAN_OS_BLOCK_INVALID;",
         "        return YAN_OS_BLOCK_MALFORMED;", 1),
    ],
    "guest-submit-checks-the-header-only": [
        # 0018 "发送前流控": only the header is measured, so half a frame can go
        # into the ring when the payload does not fit.
        ("    if (yan_os_transport_g2h_space() < frame_bytes) {",
         "    if (yan_os_transport_g2h_space() < YAN_OS_BLOCK_HEADER_SIZE) {", 1),
    ],
    "guest-acts-on-a-partial-header": [
        # 0018 rule 1 on the receiving side. With the response ring rotated so
        # its header wraps and only one byte published, the bytes behind it are
        # stale ring content; a receiver that reads them anyway decodes a count
        # no ring could hold and tries to drop a frame that was never published.
        # The row that witnesses it is "stale ring content behind a one-byte
        # first piece"; without it this mutation is equivalent to the original.
        ("    const uint32_t available = yan_os_transport_h2g_available();\n"
         "    if (available < YAN_OS_BLOCK_HEADER_SIZE) {\n"
         "        return YAN_OS_BLOCK_AGAIN;\n"
         "    }",
         "    const uint32_t available = yan_os_transport_h2g_available();\n"
         "    if (available < 1U) {\n"
         "        return YAN_OS_BLOCK_AGAIN;\n"
         "    }", 1),
    ],
    "guest-count-times-block-wraps": [
        # The product is narrowed to 32 bits before the ring check, so a count
        # that cannot fit describes a plausible frame instead of being refused.
        ("                      ? (uint64_t)load_le32(peeked + 8) * YAN_OS_BLOCK_BLOCK_SIZE",
         "                      ? (uint64_t)(load_le32(peeked + 8) * YAN_OS_BLOCK_BLOCK_SIZE)", 1),
    ],

    # ---- Guest platform: os/platform.h -------------------------------------
    "platform-h2g-pop-ignores-the-tail": [
        # The consumer position is ignored, so a response read after any earlier
        # traffic returns the wrong bytes; the rows that rotate the response
        # ring are the ones that notice. The mutation is in os/platform.h, which
        # the runner reaches through --platform, so the guest frame layer is
        # built from a copy that lives next to the mutated header.
        ("        out[i] = ring[yan_os_ring_index(tail, size, i)];",
         "        out[i] = ring[yan_os_ring_index(0, size, i)];", 1),
    ],

    # ---- Negative controls: these must NOT be counted as detections --------
    "negative-control-crash": [
        # A pure crash. Nothing was asserted, so nothing was detected.
        ("    if (block == NULL || transport == NULL) {\n"
         "        return 0;\n"
         "    }\n"
         "    RingView view;",
         "    if (block == NULL || transport == NULL) {\n"
         "        return 0;\n"
         "    }\n"
         "    *(volatile int *)0 = 1;\n"
         "    RingView view;", 1),
    ],
    "negative-control-stderr": [
        # No behaviour change at all, only a diagnostic on standard error, which
        # is also the channel sanitizer reports arrive on.
        ("#include <string.h>", "#include <stdio.h>\n#include <string.h>", 1),
        ("    uint32_t answered = 0;\n    for (;;) {",
         "    uint32_t answered = 0;\n"
         "    fputs(\"host_block: a diagnostic that changes nothing\\n\", stderr);\n"
         "    for (;;) {", 1),
    ],
    "negative-control-fixture-abort": [
        # The device cannot build itself, so the oracle never runs a check. An
        # aborted fixture is not a detection.
        ("    block->storage = storage;\n"
         "    block->capacity_blocks = capacity_blocks;\n"
         "    block->served = 0;\n"
         "    block->fail_next = false;\n"
         "    return YAN_OK;",
         "    return YAN_INVALID_ARGUMENT;", 1),
    ],
    "negative-control-guest-trap": [
        # The Guest delivers the frame, then traps. Every check it had already
        # made passed, so no expectation failed: a trap is not a failed check.
        ("    if (length != NULL) {\n"
         "        *length = (uint32_t)payload;\n"
         "    }\n"
         "    return 0;\n"
         "}",
         "    if (length != NULL) {\n"
         "        *length = (uint32_t)payload;\n"
         "    }\n"
         "    *(volatile uint32_t *)0 = 0xdeadbeefU;\n"
         "    return 0;\n"
         "}", 1),
    ],
    "negative-control-hang": [
        # A pump that never returns. Nothing is asserted, so nothing is
        # detected: the per-mutant timeout is what bounds the run, and a timeout
        # is classified as INCONCLUSIVE rather than as a detection.
        ("    uint32_t answered = 0;\n    for (;;) {",
         "    for (;;) {\n    }\n    uint32_t answered = 0;\n    for (;;) {", 1),
    ],
    "negative-control-equivalent-guard": [
        # Removing only the "fewer than 16 bytes" guard is unkillable: the frame
        # length check below refuses the same partial frames. See the header of
        # this script for the reproduction of that equivalence. If this control
        # ever starts being detected, the redundancy is gone and the table above
        # has to be revisited.
        ("        if (available < YAN_HOST_BLOCK_HEADER_SIZE) {",
         "        if (available < 1U) {", 1),
    ],
}

name = sys.argv[1]
path = sys.argv[2]
if name not in MUTATIONS:
    print("unknown mutation '%s'" % name)
    sys.exit(2)
text = open(path).read()
for old, new, expected in MUTATIONS[name]:
    found = text.count(old)
    if found != expected:
        print("pattern for '%s' occurs %d time(s), expected %d" % (name, found, expected))
        sys.exit(3)
    text = text.replace(old, new)
open(path, "w").write(text)
sys.exit(0)
APPLY_EOF

# name -> target host|guest. The drive that catches each one follows from the
# half of the protocol that is mutated: the Host's receiving rules are only
# reachable by the unit drive, which can put chosen bytes in the ring, and the
# Guest's framing is only reachable by the guest drive, which scripts the
# channel.
mutants=(
    host-acts-on-a-partial-frame
    host-consumes-the-header-only
    host-keeps-a-rejected-frame
    host-count-times-block-wraps
    host-publishes-a-partial-reply
    host-reports-blocks-on-a-failure
    host-does-not-echo-the-tag
    host-range-check-off-by-one
    host-ring-load-no-mask
    host-consumes-the-fault-flag-early
    guest-delivers-a-partial-header
    guest-consumes-the-header-only
    guest-drops-without-consuming
    guest-submit-checks-the-header-only
    guest-count-times-block-wraps
    guest-acts-on-a-partial-header
    platform-h2g-pop-ignores-the-tail
)
targets=(
    host host host host host host host host host host
    guest guest guest guest guest guest platform
)
# Which drive must witness each defect. "both" is the safe default for a Host
# mutation: the Host's receiving rules are easiest to see from the unit drive,
# which can put chosen bytes in the ring, but a defect in how the Host reads a
# *wrapped* request only shows up in the guest drive. Declaring one witness
# where the other cannot see the defect is what let host-ring-load-no-mask
# survive a gate that mapped every Host defect to the unit drive alone.
witnesses=(
    both both both both both both both both both both
    guest guest guest guest guest guest guest
)
controls=(
    negative-control-crash
    negative-control-stderr
    negative-control-fixture-abort
    negative-control-guest-trap
    negative-control-equivalent-guard
    negative-control-hang
)
control_targets=(
    host host host guest host host
)
# The hang control is about the bound on the Host unit path, so one witness is
# enough; the others are witnessed from both sides, because "not a detection"
# has to hold wherever a visitor looks.
control_witnesses=(
    both both both guest both unit
)
# The run that is supposed to hang is bounded tighter than the rest: the point
# of the control is that the bound exists and is classified, not that it waits
# for the default.
control_bounds=(
    0 0 0 0 0 20
)

# ------------------------------------------------------- the baseline suite
#
# The whole check rests on "the mutants were caught", which means nothing if the
# suite cannot pass on its own. The unmutated suite therefore runs first, in its
# full form, and a failure stops the run here.
baseline_log="$run_dir/logs/baseline.log"
echo "mutation: baseline self-check (the unmutated suite must pass)"
status=0
timeout "$timeout_s" bash "$runner" --source "$source" --gcc "$gcc" --run "$run" \
    --cc "$cc" --work "$run_dir/baseline" > "$baseline_log" 2>&1 || status=$?
if [ "$status" -ne 0 ] || grep -q ':FAIL:' "$baseline_log"; then
    echo "FAIL the baseline suite does not pass, so no mutant could be judged:"
    grep -E '^(ASSERT-FAIL|HARNESS-ERROR|NO-VERDICT|INCONCLUSIVE|BUILD-FAIL):|:FAIL:' \
        "$baseline_log" | head -n 5 | sed 's/^/    /'
    tail -n 3 "$baseline_log" | sed 's/^/    /'
    exit 1
fi
echo "mutation: baseline passed ($(tail -n 1 "$baseline_log"))"

# ------------------------------------------------------------------ the loop

# $1 log, $2 the suite's exit status. Prints the class.
classify() {
    local log="$1" status="$2"
    if grep -q '^BUILD-FAIL:' "$log"; then
        echo "BUILD-FAIL"
    elif [ "$status" -eq 124 ]; then
        echo "TIMEOUT"
    elif grep -q '^ASSERT-FAIL:' "$log" && grep -q ':FAIL:' "$log"; then
        echo "DETECTED"
    elif grep -q '^HARNESS-ERROR:' "$log" || grep -q '^NO-VERDICT:' "$log" ||
         grep -q '^INCONCLUSIVE:' "$log"; then
        echo "INCONCLUSIVE"
    else
        echo "SURVIVED"
    fi
}

# $1 name, $2 target, $3 witnesses (unit|guest|both), $4 output log,
# [$5 timeout]; prints the strongest class any witness produced.
plant_and_run() {
    local name="$1" target="$2" want="$3" log="$4" bound="${5:-}"
    [ -n "$bound" ] && [ "$bound" != "0" ] || bound="$timeout_s"
    local original override copy
    case "$target" in
    host)
        original="$host_block"
        copy="$run_dir/copy/$name/host_block.c"
        override="--host-block $copy" ;;
    guest)
        original="$guest_block"
        copy="$run_dir/copy/$name/block.c"
        override="--block $copy" ;;
    platform)
        # os/platform.h is reached through a quoted include that resolves next
        # to the file including it, so the override ships the matching
        # os/block.c in the same directory and the runner puts that directory
        # first on the include path.
        mkdir -p "$run_dir/copy/$name/os"
        cp "$source/os/block.c" "$run_dir/copy/$name/os/block.c"
        original="$source/os/platform.h"
        copy="$run_dir/copy/$name/os/platform.h"
        override="--block $run_dir/copy/$name/os/block.c --platform $copy" ;;
    esac
    mkdir -p "$(dirname "$copy")"
    cp "$original" "$copy"
    local before after
    before="$(md5sum < "$copy")"
    if ! python3 "$run_dir/apply.py" "$name" "$copy" > "$run_dir/logs/$name-apply.log" 2>&1; then
        echo "APPLY-FAIL"
        return 0
    fi
    after="$(md5sum < "$copy")"
    if [ "$before" = "$after" ]; then
        echo "APPLY-FAIL"
        return 0
    fi
    : > "$log"
    local best="SURVIVED" witness mode status class
    local ran_modes=0
    for mode in unit guest; do
        if [ "$want" != "both" ] && [ "$want" != "$mode" ]; then
            continue
        fi
        ran_modes=$((ran_modes + 1))
        local wlog="$run_dir/logs/$name-$mode.log"
        status=0
        # shellcheck disable=SC2086
        timeout "$bound" bash "$runner" --source "$source" --gcc "$gcc" --run "$run" \
            --cc "$cc" --work "$run_dir/work-$name-$mode" --drive "$mode" \
            $override > "$wlog" 2>&1 || status=$?
        class="$(classify "$wlog" "$status")"
        { echo "--- witness $mode: $class ---"; cat "$wlog"; } >> "$log"
        case "$class" in
            DETECTED)
                echo "witness $mode" >> "$log"
                echo "DETECTED"
                return 0 ;;
            BUILD-FAIL|TIMEOUT|INCONCLUSIVE)
                [ "$best" = "SURVIVED" ] && best="$class" ;;
        esac
    done
    # A mutation that no drive was asked to witness is a wiring mistake, and it
    # must not be able to look like a survivor.
    if [ "$ran_modes" -eq 0 ]; then
        echo "HARNESS-ERROR"
        return 0
    fi
    echo "$best"
}

detected=0
missed=0
controls_ok=0
controls_bad=0

echo "mutation: ${#mutants[@]} planted defect(s)"
for index in "${!mutants[@]}"; do
    name="${mutants[index]}"
    [ -z "$only" ] || [ "$only" = "$name" ] || continue
    log="$run_dir/logs/$name.log"
    class="$(plant_and_run "$name" "${targets[index]}" "${witnesses[index]}" "$log")"
    if [ "$class" = "DETECTED" ]; then
        catcher="$(grep -m 1 '^ASSERT-FAIL:' "$log" | sed 's/^ASSERT-FAIL: //')"
        marks="$(grep -c ':FAIL:' "$log")"
        witness="$(sed -n 's/^witness //p' "$log" | tail -n 1)"
        echo "PASS mutation $name DETECTED by $catcher ($marks failed-check line(s), witness $witness)"
        detected=$((detected + 1))
    else
        echo "FAIL mutation $name was not detected ($class)"
        grep -E '^(ASSERT-FAIL|HARNESS-ERROR|NO-VERDICT|INCONCLUSIVE|BUILD-FAIL|TRAP|UNEXPECTED):' \
            "$log" | head -n 3 | sed 's/^/    /'
        [ "$class" = "APPLY-FAIL" ] && sed 's/^/    /' "$run_dir/logs/$name-apply.log"
        missed=$((missed + 1))
    fi
done

echo "mutation: ${#controls[@]} negative control(s), none of which may be a detection"
for index in "${!controls[@]}"; do
    name="${controls[index]}"
    [ -z "$only" ] || [ "$only" = "$name" ] || continue
    log="$run_dir/logs/$name.log"
    class="$(plant_and_run "$name" "${control_targets[index]}" \
        "${control_witnesses[index]}" "$log" "${control_bounds[index]}")"
    if [ "$class" = "DETECTED" ]; then
        echo "FAIL negative control $name was counted as a detection:"
        grep -m 2 '^ASSERT-FAIL:' "$log" | sed 's/^/    /'
        controls_bad=$((controls_bad + 1))
    else
        echo "PASS negative control $name is not a detection ($class)"
        controls_ok=$((controls_ok + 1))
    fi
done

# The originals must be exactly as they were: every defect was planted in a copy.
host_after="$(md5sum < "$host_block")"
guest_after="$(md5sum < "$guest_block")"
if [ "$host_before" != "$host_after" ] || [ "$guest_before" != "$guest_after" ]; then
    echo "FAIL the mutation check modified the sources it copies"
    missed=$((missed + 1))
else
    echo "mutation: every defect was planted in a copy; tools/host_block.c and os/block.c are unchanged"
fi

if [ "$keep" -eq 0 ]; then
    rm -rf "$run_dir/copy" "$run_dir/work" "$run_dir/baseline"
fi

echo "mutation: $detected detected, $missed not detected, $controls_ok control(s) behaved, $controls_bad control(s) miscounted"
if [ "$missed" -ne 0 ] || [ "$controls_bad" -ne 0 ]; then
    echo "FAIL the block protocol mutation check did not catch every planted defect"
    exit 1
fi
echo "PASS every planted defect was caught by an assertion and no control was miscounted"
exit 0
