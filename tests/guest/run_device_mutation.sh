#!/usr/bin/env bash
# Mutation check for the device layer: the UART character device (0015), the
# host transport channel (0014), the platform sampling that maps their IRQ lines
# onto PLIC sources 1 and 2, and the PLIC gateway state machine itself (0016).
#
# Why this exists: these mutation checks used to be run by hand in throw-away
# copies of the tree, so nothing in the repository could reproduce them. This
# script plants one deliberate defect at a time in a private copy of the tree,
# rebuilds, runs the oracle, and requires every mutation to be caught.
#
# What the oracle is: the 21 Host CTest suites plus a generated probe suite that
# this script writes into the work copy. It is NOT the whole CTest suite. The
# inner configure passes no -DYAN_BUILD_TOOLS, so the work copy does not
# register the Guest end-to-end suites (guest_trap_env, guest_terminal,
# guest_console); they exercise the console driver through yan_run rather than
# the device itself, and guest_console alone costs about ten seconds per mutant,
# which is about five minutes over the 28 mutants. --tools builds the work copy
# with -DYAN_BUILD_TOOLS=ON and adds them when that price is worth paying.
#
# Detection criterion (deliberately strict): a mutation counts as DETECTED only
# when a test fails on an ASSERTION - the failing binary must print Unity's
# ":FAIL:" marker. A crash, a timeout, a non-zero exit without an assertion, or
# a sanitizer report does NOT count: an earlier round had a mutation that looked
# caught only because UBSan trapped undefined behaviour, and it survived once
# the UB was removed. Those cases are printed separately as non-assertion
# failures so a false sense of coverage cannot hide behind them.
#
# The probe suite exists because the standing suites leave some documented
# invariants unpinned: a backend whose readiness changes between the STATUS read
# and the TXDATA write, a detached device that still holds a byte, publish(0)
# and DOORBELL as commands that must not judge anything, guest ring pointers
# that were never normalised, and the platform owning PLIC sources 1 and 2.
#
# The inner CTest runs exclude two tests by default, and the exclusion is
# explicit on purpose: it must not rest on the accident that the inner
# configure happens to leave YAN_ENABLE_MUTATION_TESTS (and YAN_BUILD_TOOLS)
# unset. device_mutation is this script's own registered CTest entry, so a work
# copy that did register it would make the check recurse into itself. The
# other, guest_console_mutation, is itself a mutation harness: nesting a
# 28-mutant sweep inside every mutant adds no oracle for device defects and
# would dominate the runtime. Everything else in the inner suite runs for every
# mutant. --ignore adds names (a regex) for unrelated in-flight work: an
# excluded test never runs, in the baseline or in a mutant run, so it can
# neither block the check nor be mistaken for a detection.
#
# Proof that a mutant was really built: before every build the objects of the
# four mutated translation units are deleted, so the compiler cannot skip one on
# a timestamp comparison. The md5 check proves the edit landed in the file; the
# deletion proves the build read the edited file.
#
# Anchors: every mutation is one literal old -> new replacement whose pattern
# must occur exactly once (twice for the two host entry points), and the script
# proves the edit landed by comparing md5 sums before and after. A pattern is
# written as the shortest unique fragment that carries the decision, without
# leading indentation, so re-indenting the surrounding block does not break it.
# A few mutations add or remove whole statements and are still block patterns;
# those assume the block's current layout. When a refactor moves one of them,
# update the anchor here - production code is never reshaped to fit this
# harness, and a mutation whose code shape disappears is retired, not rewritten
# into an equivalent.
#
# Exit codes: 0 every mutation was caught by an assertion, 1 a mutation survived
# (survivors are printed separately), 2 usage error, 77 a dependency is missing.
#
# Usage:
#   tests/guest/run_device_mutation.sh --source DIR --work DIR \
#       [--target uart|transport|platform|plic|all] [--unity DIR] [--jobs N] \
#       [--ignore REGEX] [--sanitizers] [--keep]
#
#   --source DIR   repository to copy; it is never modified
#   --work DIR     scratch root; each run creates and rewrites its own
#                  run.<pid> directory inside it, so a shared root (which the
#                  CMake registration uses) is safe for concurrent runs
#   --target NAME  uart, transport, platform, plic or all (default: all)
#   --unity DIR    Unity source tree; without it an existing
#                  $source/build/*/_deps/unity-src is reused, otherwise CMake
#                  fetches it (which needs network access)
#   --jobs N       parallel build and CTest jobs (default: the machine's CPU
#                  count)
#   --ignore REGEX exclude tests whose names match REGEX from every CTest run
#                  in this check, for unrelated in-flight failures; excluded
#                  tests are never evidence
#   --exclude REGEX replace the default exclusion list (device_mutation and
#                  guest_console_mutation) instead of adding to it
#   --sanitizers   build with -DYAN_ENABLE_SANITIZERS=ON; the detection
#                  criterion stays "assertion failure", so sanitizer-only
#                  catches are still reported as survivors
#   --tools        build the work copy with -DYAN_BUILD_TOOLS=ON so the Guest
#                  end-to-end suites join the oracle; they add roughly ten
#                  seconds to every mutant run, so the default keeps them out
#   --keep         keep the work tree after the run
#   --gcc / --run  accepted for command-line parity with the other Guest
#                  runners; a Host-side device check does not use them
#
# The environment variable YAN_DEVICE_MUTATION_CTEST_TIMEOUT (seconds, default
# 900) bounds every inner CTest run.
set -u

source=""
work=""
target="all"
unity=""
jobs=""
sanitizers=0
tools=0
keep=0
ignore=""
exclude=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --target) target="$2"; shift 2 ;;
        --unity) unity="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --sanitizers) sanitizers=1; shift ;;
        --tools) tools=1; shift ;;
        --ignore) ignore="$2"; shift 2 ;;
        --exclude) exclude="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        --gcc|--run) shift 2 ;;
        *) echo "run_device_mutation.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$source" ] || [ -z "$work" ]; then
    echo "usage: run_device_mutation.sh --source DIR --work DIR [--target uart|transport|platform|plic|all] [--unity DIR] [--jobs N] [--sanitizers] [--keep]" >&2
    exit 2
fi
case "$target" in
    uart|transport|platform|plic|all) ;;
    *) echo "run_device_mutation.sh: --target must be uart, transport, platform, plic or all" >&2; exit 2 ;;
esac
[ -z "$jobs" ] && jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# device_mutation is this script's own CTest entry in the work copy: running it
# from the inside would recurse. guest_console_mutation is a nested mutation
# harness. --ignore adds to the list, --exclude replaces it.
if [ -z "$exclude" ]; then
    exclude="device_mutation|guest_console_mutation"
fi
if [ -n "$ignore" ]; then
    exclude="$exclude|$ignore"
fi

# YAN_BUILD_TOOLS is off by default: the Guest end-to-end suites exercise the
# console driver rather than the device, and their own mutation harness runs
# longer than the rest of the suite. --tools adds them to the oracle.
tools_flag="OFF"
[ "$tools" -eq 1 ] && tools_flag="ON"

for required in "$source/src/uart.c" "$source/src/transport.c" "$source/src/machine.c" \
                "$source/src/interrupt.c" \
                "$source/tests/test_uart.c" "$source/tests/test_transport.c" \
                "$source/CMakeLists.txt"; do
    [ -e "$required" ] || { echo "SKIP: missing $required"; exit 77; }
done
command -v cmake >/dev/null 2>&1 || { echo "SKIP: cmake not found"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 77; }

# Reuse a Unity checkout an earlier build already fetched: the pin lives in
# CMakeLists.txt, so an existing one is the same revision.
if [ -z "$unity" ]; then
    for candidate in "$source"/build/*/_deps/unity-src; do
        [ -d "$candidate" ] && unity="$candidate" && break
    done
fi
unity_arg=""
if [ -n "$unity" ]; then
    [ -d "$unity" ] || { echo "SKIP: --unity '$unity' is not a directory"; exit 77; }
    unity_arg="-DFETCHCONTENT_SOURCE_DIR_UNITY=$unity"
fi

# One private directory per run: $work is shared by design (the CMake
# registration points every mutation check at one root), so the tree, the build
# and the logs live under run.<pid>. Two concurrent runs then cannot delete each
# other's sources, which is exactly what happened once with a fixed path.
run="$work/run.$$"
mkdir -p "$run"
tree="$run/tree"
build="$tree/build/device-mutation"

echo "device mutation check"
echo "  source    : $source"
echo "  work      : $work"
echo "  target    : $target"
echo "  unity     : ${unity:-<fetched by CMake>}"
echo "  sanitizers: $([ "$sanitizers" -eq 1 ] && echo on || echo off)"
echo "  tools     : $tools_flag"
echo "  excluded  : $exclude"

rm -rf "$tree"
mkdir -p "$tree"
# The work copy excludes build/ and .git/: they are large and irrelevant, and
# nothing below ever writes to $source.
tar -C "$source" --exclude=./build --exclude=./.git -cf - . | tar -C "$tree" -xf - \
    || { echo "SKIP: cannot copy $source into $tree"; exit 77; }
[ -f "$tree/src/uart.c" ] || { echo "SKIP: the copy has no src/uart.c"; exit 77; }

mkdir -p "$run/pristine"
cp -a "$tree/src/uart.c" "$tree/src/transport.c" "$tree/src/machine.c" \
      "$tree/src/interrupt.c" "$run/pristine/"

# --------------------------------------------------------------- probe suite

# The probe is written into the copy, never into $source, and registered through
# the appended CMake block below.
cat > "$tree/tests/test_device_mutation_probe.c" <<'PROBE'
/* Generated by tests/guest/run_device_mutation.sh inside its work copy: the
 * oracle for the invariants the standing suites leave open. See
 * docs/specs/0014-host-transport-channel.md and 0015-uart-device.md. */
#include "yan/bus.h"
#include "yan/machine.h"
#include "yan/transport.h"
#include "yan/uart.h"
#include "unity.h"

/* ------------------------------------------------------------- UART probe */

typedef struct {
    bool answers[4];
    size_t answer_count;
    size_t ready_calls;
    size_t write_calls;
    uint8_t last_byte;
    bool wrote_while_not_ready;
} Scripted;

static bool scripted_ready(void *context)
{
    Scripted *self = context;
    size_t index = self->ready_calls < self->answer_count ? self->ready_calls
                                                          : self->answer_count - 1;
    ++self->ready_calls;
    return self->answers[index];
}

static void scripted_write(void *context, uint8_t byte)
{
    Scripted *self = context;
    if (self->ready_calls == 0) {
        self->wrote_while_not_ready = true;
    } else {
        size_t index = self->ready_calls - 1;
        if (index >= self->answer_count) {
            index = self->answer_count - 1;
        }
        if (!self->answers[index]) {
            self->wrote_while_not_ready = true;
        }
    }
    ++self->write_calls;
    self->last_byte = byte;
}

typedef struct {
    bool ready;
    size_t count;
    uint8_t bytes[16];
} Sink;

static Sink sink;

static bool sink_ready(void *context)
{
    (void)context;
    return sink.ready;
}

static void sink_write(void *context, uint8_t byte)
{
    (void)context;
    if (sink.count < sizeof(sink.bytes)) {
        sink.bytes[sink.count] = byte;
    }
    ++sink.count;
}

#define UART_REG(offset) (YAN_UART_BASE + (offset))
#define CH_REG(offset) (YAN_TRANSPORT_BASE + (offset))
#define RAM_BASE UINT32_C(0x80000000)
#define RAM_BYTES 512U
#define RING_BASE RAM_BASE
#define RING_SIZE UINT32_C(64)

static YanRam ram;
static YanBus bus;
static YanUart uart;
static YanTransport channel;
static YanMachine machine;
static unsigned notify_calls;

static void counting_notify(void *context)
{
    (void)context;
    ++notify_calls;
}

static void attach_sink(YanUart *target, bool ready)
{
    sink = (Sink){0};
    sink.ready = ready;
    const YanUartTerminal terminal = {&sink, sink_ready, sink_write};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(target, &terminal));
}

static uint32_t uart_reg(uint32_t offset)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_read(&bus, UART_REG(offset), 4, &value).status);
    return value;
}

static YanStatus uart_reg_write(uint32_t offset, uint32_t value)
{
    return yan_bus_write(&bus, UART_REG(offset), 4, value).status;
}

static uint32_t ch_reg(uint32_t offset)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_read(&bus, CH_REG(offset), 4, &value).status);
    return value;
}

static void ch_reg_write(uint32_t offset, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, CH_REG(offset), 4, value).status);
}

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    uart = (YanUart){0};
    channel = (YanTransport){0};
    machine = (YanMachine){0};
    notify_calls = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, RAM_BYTES));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, RAM_BASE));
    bus.uart = &uart;
    bus.transport = &channel;
    yan_uart_reset(&uart);
    yan_transport_reset(&channel);
}

void tearDown(void)
{
    yan_machine_destroy(&machine);
    yan_ram_destroy(&ram);
}

static void channel_configure(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_configure(&channel, RING_BASE, RING_SIZE,
                                                          RAM_BASE, RAM_BYTES));
    yan_transport_set_notify(&channel, counting_notify, &channel);
}

/* The STATUS bit and the TXDATA write path must sample the same live answer.
 * The specification allows the backend to change between the two calls, and
 * then demands a refusal instead of a silent drop - and never a byte handed to
 * a sink that said no. */
static void uart_ready_bit_and_write_agree_on_the_live_answer(void)
{
    Scripted backend = {0};
    const YanUartTerminal terminal = {&backend, scripted_ready, scripted_write};
    const bool samples[4] = {true, false, true, false};
    const bool at_write[4] = {true, false, false, true};

    for (int index = 0; index < 4; ++index) {
        backend = (Scripted){0};
        backend.answers[0] = samples[index];
        backend.answers[1] = at_write[index];
        backend.answer_count = 2;
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, &terminal));

        const bool advertised = (uart_reg(YAN_UART_STATUS) & YAN_UART_STATUS_TX_READY) != 0;
        TEST_ASSERT_EQUAL_INT(samples[index], advertised);
        const YanStatus status = uart_reg_write(YAN_UART_TXDATA, UINT32_C(0x1ab));
        TEST_ASSERT_FALSE(backend.wrote_while_not_ready);
        if (at_write[index]) {
            TEST_ASSERT_EQUAL_INT(YAN_OK, status);
            TEST_ASSERT_EQUAL_size_t(1, backend.write_calls);
            TEST_ASSERT_EQUAL_HEX8(0xab, backend.last_byte);
        } else {
            TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, status);
            TEST_ASSERT_EQUAL_size_t(0, backend.write_calls);
        }
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, NULL));
    }
}

/* A disconnected device reports zero on all three bits even when a byte is
 * still in the buffer - the state the specification names explicitly. */
static void uart_disconnected_reports_zero_with_a_stale_buffer(void)
{
    uart.terminal_attached = false;
    uart.rx_data = 0x5a;
    uart.rx_available = true;
    uart.irq_status = YAN_UART_IRQ_RX_PENDING;
    uart.control = YAN_UART_CONTROL_RX_IRQ_ENABLE;

    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_STATUS));
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, uart_reg_write(YAN_UART_TXDATA, 'A'));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_uart_push_rx(&uart, 0x41));
}

/* The receive line is RX_READY && RX_IRQ_ENABLE over every reachable
 * combination, including the detached-but-buffered one. */
static void uart_irq_line_truth_table(void)
{
    for (int attached = 0; attached < 2; ++attached) {
        for (int buffered = 0; buffered < 2; ++buffered) {
            for (int enable = 0; enable < 2; ++enable) {
                yan_uart_reset(&uart);
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, NULL));
                if (attached) {
                    attach_sink(&uart, true);
                }
                if (buffered) {
                    if (attached) {
                        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x33));
                    } else {
                        uart.rx_available = true;
                        uart.rx_data = 0x33;
                    }
                }
                uart.control = enable ? YAN_UART_CONTROL_RX_IRQ_ENABLE : 0;
                const uint32_t status = uart_reg(YAN_UART_STATUS);
                TEST_ASSERT_EQUAL_INT(attached != 0,
                                      (status & YAN_UART_STATUS_CONNECTED) != 0);
                TEST_ASSERT_EQUAL_INT(attached != 0,
                                      (status & YAN_UART_STATUS_TX_READY) != 0);
                TEST_ASSERT_EQUAL_INT(attached && buffered,
                                      (status & YAN_UART_STATUS_RX_READY) != 0);
                TEST_ASSERT_EQUAL_INT(attached && buffered && enable,
                                      yan_uart_pending(&uart));
            }
        }
    }
}

static void uart_disconnect_clears_the_buffer_but_keeps_control(void)
{
    attach_sink(&uart, true);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          uart_reg_write(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x5a));
    TEST_ASSERT_TRUE(yan_uart_pending(&uart));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, NULL));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_CONTROL_RX_IRQ_ENABLE, uart_reg(YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_IRQ_STATUS));

    /* Only reconnecting shows whether the byte is really gone: while detached
     * RX_READY reads zero either way. */
    attach_sink(&uart, true);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            uart_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_RXDATA));
    TEST_ASSERT_FALSE(uart.rx_available);
}

static void uart_read_rxdata_clears_rx_ready(void)
{
    attach_sink(&uart, true);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x77));
    TEST_ASSERT_TRUE((uart_reg(YAN_UART_STATUS) & YAN_UART_STATUS_RX_READY) != 0);
    TEST_ASSERT_EQUAL_HEX32(0x77, uart_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_STATUS) & YAN_UART_STATUS_RX_READY);
    TEST_ASSERT_FALSE(uart.rx_available);
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_RXDATA));

    /* A full buffer refuses a second byte instead of overwriting it. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x11));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_uart_push_rx(&uart, 0x22));
    TEST_ASSERT_EQUAL_HEX8(0x11, uart.rx_data);
    TEST_ASSERT_EQUAL_HEX32(0x11, uart_reg(YAN_UART_RXDATA));
}

static void uart_rejects_half_a_backend_and_keeps_the_connection(void)
{
    attach_sink(&uart, true);
    const YanUartTerminal no_write = {&sink, sink_ready, NULL};
    const YanUartTerminal no_ready = {&sink, NULL, sink_write};
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_uart_set_terminal(&uart, &no_write));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_uart_set_terminal(&uart, &no_ready));
    TEST_ASSERT_TRUE(yan_uart_connected(&uart));
    TEST_ASSERT_EQUAL_INT(YAN_OK, uart_reg_write(YAN_UART_TXDATA, 'K'));
    TEST_ASSERT_EQUAL_size_t(1, sink.count);
    TEST_ASSERT_EQUAL_HEX8('K', sink.bytes[0]);
}

static void uart_reset_keeps_the_backend(void)
{
    attach_sink(&uart, true);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          uart_reg_write(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x41));
    yan_uart_reset(&uart);
    TEST_ASSERT_TRUE(yan_uart_connected(&uart));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            uart_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, uart_reg(YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_INT(YAN_OK, uart_reg_write(YAN_UART_TXDATA, 'R'));
    TEST_ASSERT_EQUAL_size_t(1, sink.count);
}

/* -------------------------------------------------------- transport probe */

static void channel_capacity_is_size_minus_one(void)
{
    channel_configure();
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, RING_SIZE - 1));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_publish(&channel, 1));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, channel.h2g_head);

    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_STATUS_G2H_FULL,
                            ch_reg(YAN_TRANSPORT_STATUS) &
                                YAN_TRANSPORT_STATUS_G2H_FULL);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_consume(&channel, RING_SIZE - 1));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
}

static void channel_publish_zero_is_not_data(void)
{
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_IRQ_ENABLE, 1);

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 0));
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, channel.irq_status);
    TEST_ASSERT_FALSE(yan_transport_pending(&channel));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_STATUS_HOST_READY |
                                YAN_TRANSPORT_STATUS_H2G_EMPTY,
                            ch_reg(YAN_TRANSPORT_STATUS));

    /* Real data still raises the line, so the check above is not vacuous. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 1));
    TEST_ASSERT_TRUE(yan_transport_pending(&channel));
    TEST_ASSERT_EQUAL_HEX32(0, ch_reg(YAN_TRANSPORT_STATUS) &
                                   YAN_TRANSPORT_STATUS_H2G_EMPTY);
}

static void channel_doorbell_decides_nothing(void)
{
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_readable(&channel));

    ch_reg_write(YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_UINT(1, notify_calls);
    TEST_ASSERT_EQUAL_INT(0, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(0, ch_reg(YAN_TRANSPORT_STATUS) &
                                   YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED);

    /* An impossible head is still not judged at the doorbell. */
    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, UINT32_C(0xffffffff));
    ch_reg_write(YAN_TRANSPORT_DOORBELL, 7);
    TEST_ASSERT_EQUAL_UINT(2, notify_calls);
    TEST_ASSERT_EQUAL_INT(0, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xffffffff), channel.g2h_head);

    /* The accounting API is where the overrun is decided. */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE,
                          yan_transport_host_consume(&channel, RING_SIZE));
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);
}

static void channel_unavailable_paths_are_not_faults(void)
{
    YanTransport bare = {0};
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_consume(&bare, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_consume(&bare, 1));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_publish(&bare, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_publish(&bare, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(0, bare.overflow_detected);

    /* A ring without a callback is the other half of the same gap. */
    bare.ring_base = RING_BASE;
    bare.ring_size = RING_SIZE;
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_consume(&bare, 1));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_publish(&bare, 1));
    TEST_ASSERT_EQUAL_INT(0, bare.overflow_detected);
}

/* Read-only and host-driven registers swallow a write: the guest may not move
 * a pointer the host owns, and the identity registers are constants. This keeps
 * the probe self-sufficient if the standing transport suite is refactored. */
static void channel_read_only_writes_change_nothing(void)
{
    channel_configure();
    const uint32_t magic = ch_reg(YAN_TRANSPORT_MAGIC_REG);

    ch_reg_write(YAN_TRANSPORT_G2H_TAIL, 7);
    TEST_ASSERT_EQUAL_HEX32(0, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(0, ch_reg(YAN_TRANSPORT_G2H_TAIL));
    ch_reg_write(YAN_TRANSPORT_H2G_HEAD, 9);
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, ch_reg(YAN_TRANSPORT_H2G_HEAD));
    ch_reg_write(YAN_TRANSPORT_RING_BASE, 0);
    ch_reg_write(YAN_TRANSPORT_RING_SIZE, 4096);
    TEST_ASSERT_EQUAL_HEX32(RING_BASE, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE, channel.ring_size);
    ch_reg_write(YAN_TRANSPORT_MAGIC_REG, 0);
    TEST_ASSERT_EQUAL_HEX32(magic, ch_reg(YAN_TRANSPORT_MAGIC_REG));
    ch_reg_write(YAN_TRANSPORT_STATUS, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_STATUS_HOST_READY |
                                YAN_TRANSPORT_STATUS_H2G_EMPTY,
                            ch_reg(YAN_TRANSPORT_STATUS));
}

static void channel_overrun_moves_nothing(void)
{
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, 3);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_consume(&channel, 4));
    TEST_ASSERT_EQUAL_HEX32(0, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(3, ch_reg(YAN_TRANSPORT_G2H_HEAD));

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_publish(&channel, 64));
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, ch_reg(YAN_TRANSPORT_H2G_TAIL));
}

/* The guest pointers are not normalised by the device, and the host-side
 * comparison has to stay correct modulo RING_SIZE in both directions. */
static void channel_compares_unnormalised_pointers_modulo(void)
{
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, UINT32_C(0xfffffff0));
    TEST_ASSERT_EQUAL_HEX32(48, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_consume(&channel, 49));
    TEST_ASSERT_EQUAL_HEX32(0, channel.g2h_tail);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_consume(&channel, 48));
    TEST_ASSERT_EQUAL_HEX32(48, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));

    yan_transport_reset(&channel);
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_H2G_TAIL, UINT32_C(0xffffffff));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 2, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_transport_host_publish(&channel, RING_SIZE - 2));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_publish(&channel, 1));
}

static void channel_rings_do_not_share_state(void)
{
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, 5);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 3));
    TEST_ASSERT_EQUAL_HEX32(3, channel.h2g_head);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_consume(&channel, 5));
    TEST_ASSERT_EQUAL_HEX32(5, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(3, channel.h2g_head);

    const uint32_t tail_before = channel.g2h_tail;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_publish(&channel, 64));
    TEST_ASSERT_EQUAL_HEX32(tail_before, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(3, channel.h2g_head);
}

static void channel_reset_keeps_configuration_and_clears_the_latch(void)
{
    channel_configure();
    ch_reg_write(YAN_TRANSPORT_G2H_HEAD, 9);
    ch_reg_write(YAN_TRANSPORT_IRQ_ENABLE, 1);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_consume(&channel, 10));
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);

    yan_transport_reset(&channel);
    TEST_ASSERT_EQUAL_INT(0, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(0, channel.irq_status);
    TEST_ASSERT_EQUAL_HEX32(RING_BASE, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE, channel.ring_size);
    TEST_ASSERT_EQUAL_PTR(counting_notify, channel.notify);
}

/* ------------------------------------------------------ PLIC gateway */

/* The gateway rows of docs/specs/0016: level, pending, in-service, enable,
 * priority and threshold are separate states, and each row is checked. */
static void plic_gateway_follows_the_level(void)
{
    YanPlic plic = {0};
    const uint32_t source = 3;
    const uint32_t bit = UINT32_C(1) << source;
    uint32_t claimed = 0;
    yan_plic_reset(&plic);
    plic.priority[source] = 1;
    plic.enable_m = bit;

    /* An unclaimed assertion pends the source; a deassertion withdraws it. */
    yan_plic_set_level(&plic, source, true);
    TEST_ASSERT_EQUAL_HEX32(bit, plic.pending);
    yan_plic_set_level(&plic, source, false);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    yan_plic_set_level(&plic, source, true);

    /* Claim clears pending and marks it in service; the summary follows. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_read(&plic, YAN_PLIC_CLAIM_M, &claimed));
    TEST_ASSERT_EQUAL_HEX32(source, claimed);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(bit, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));

    /* While it is in service, further assertions must not re-pend it (I1). */
    yan_plic_set_level(&plic, source, true);
    yan_plic_set_level(&plic, source, true);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));

    /* Completing re-samples the level: still asserted means pended again. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_write(&plic, YAN_PLIC_CLAIM_M, source));
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(bit, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));

    /* A handler that has already lowered the level leaves nothing behind. */
    yan_plic_set_level(&plic, source, false);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    yan_plic_set_level(&plic, source, true);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_read(&plic, YAN_PLIC_CLAIM_M, &claimed));
    TEST_ASSERT_EQUAL_HEX32(source, claimed);
    yan_plic_set_level(&plic, source, false);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_write(&plic, YAN_PLIC_CLAIM_M, source));
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
}

static void plic_gateway_arbitrates_on_enable_priority_and_threshold(void)
{
    YanPlic plic = {0};
    const uint32_t source = 3;
    const uint32_t bit = UINT32_C(1) << source;
    uint32_t claimed = 0;

    yan_plic_reset(&plic);
    yan_plic_set_level(&plic, source, true);

    /* Priority zero never interrupts, whatever the enable says. */
    plic.enable_m = bit;
    TEST_ASSERT_EQUAL_HEX32(bit, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_read(&plic, YAN_PLIC_CLAIM_M, &claimed));
    TEST_ASSERT_EQUAL_HEX32(0, claimed);

    /* Enabled and given a priority, it selects. */
    plic.priority[source] = 2;
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_read(&plic, YAN_PLIC_CLAIM_M, &claimed));
    TEST_ASSERT_EQUAL_HEX32(source, claimed);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_write(&plic, YAN_PLIC_CLAIM_M, source));

    /* Disabled again: the request stays pending but never reaches MEIP. */
    plic.enable_m = 0;
    TEST_ASSERT_EQUAL_HEX32(bit, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_read(&plic, YAN_PLIC_CLAIM_M, &claimed));
    TEST_ASSERT_EQUAL_HEX32(0, claimed);

    /* A threshold at or above the priority gates it out. */
    plic.enable_m = bit;
    plic.threshold_m = 2;
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_plic_read(&plic, YAN_PLIC_CLAIM_M, &claimed));
    TEST_ASSERT_EQUAL_HEX32(0, claimed);
    plic.threshold_m = 1;
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));
}

/* ------------------------------------------------------- platform mapping */

static void platform_maps_uart_to_source_two_and_transport_to_source_one(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_transport_configure(&machine.transport, YAN_RAM_BASE,
                                                  UINT32_C(256), YAN_RAM_BASE,
                                                  YAN_RAM_SIZE));
    yan_transport_set_notify(&machine.transport, counting_notify, &machine);
    attach_sink(&machine.uart, true);

    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&machine.bus, YAN_UART_BASE + YAN_UART_CONTROL, 4,
                                        YAN_UART_CONTROL_RX_IRQ_ENABLE).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&machine.uart, 0x41));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&machine.bus,
                                        YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 4,
                                        1).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&machine.transport, 1));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32((UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART) |
                                (UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_TRANSPORT),
                            machine.plic.pending);

    /* The platform drives both directions: a caller's own level is replaced on
     * the next sample, and a quiet device withdraws its source. */
    yan_plic_set_level(&machine.plic, YAN_MACHINE_PLIC_SOURCE_UART, false);
    yan_plic_set_level(&machine.plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT, false);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32((UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART) |
                                (UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_TRANSPORT),
                            machine.plic.pending);

    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_read(&machine.bus, YAN_UART_BASE + YAN_UART_RXDATA, 4,
                                       &(uint32_t){0}).status);
    TEST_ASSERT_EQUAL_HEX32(0, yan_uart_pending(&machine.uart));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            machine.plic.pending);
}

static void platform_never_raises_meip_without_enable_and_priority(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_transport_configure(&machine.transport, YAN_RAM_BASE,
                                                  UINT32_C(256), YAN_RAM_BASE,
                                                  YAN_RAM_SIZE));
    yan_transport_set_notify(&machine.transport, counting_notify, &machine);
    attach_sink(&machine.uart, true);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&machine.bus, YAN_UART_BASE + YAN_UART_CONTROL, 4,
                                        YAN_UART_CONTROL_RX_IRQ_ENABLE).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&machine.uart, 0x52));

    /* The line is asserted; nothing reaches MEIP without the controller. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&machine.plic));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mip & YAN_INTERRUPT_MEIP);

    /* Enable, priority still zero. */
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&machine.bus, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M, 4,
                                        UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));

    /* Both halves: the platform line finally becomes MEIP. */
    TEST_ASSERT_EQUAL_INT(
        YAN_OK,
        yan_bus_write(&machine.bus,
                      YAN_PLIC_BASE + YAN_PLIC_PRIORITY +
                          4 * YAN_MACHINE_PLIC_SOURCE_UART,
                      4, 1).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_bus_pending_interrupts(&machine.bus));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(uart_ready_bit_and_write_agree_on_the_live_answer);
    RUN_TEST(uart_disconnected_reports_zero_with_a_stale_buffer);
    RUN_TEST(uart_irq_line_truth_table);
    RUN_TEST(uart_disconnect_clears_the_buffer_but_keeps_control);
    RUN_TEST(uart_read_rxdata_clears_rx_ready);
    RUN_TEST(uart_rejects_half_a_backend_and_keeps_the_connection);
    RUN_TEST(uart_reset_keeps_the_backend);
    RUN_TEST(channel_capacity_is_size_minus_one);
    RUN_TEST(channel_publish_zero_is_not_data);
    RUN_TEST(channel_doorbell_decides_nothing);
    RUN_TEST(channel_unavailable_paths_are_not_faults);
    RUN_TEST(channel_read_only_writes_change_nothing);
    RUN_TEST(channel_overrun_moves_nothing);
    RUN_TEST(channel_compares_unnormalised_pointers_modulo);
    RUN_TEST(channel_rings_do_not_share_state);
    RUN_TEST(channel_reset_keeps_configuration_and_clears_the_latch);
    RUN_TEST(plic_gateway_follows_the_level);
    RUN_TEST(plic_gateway_arbitrates_on_enable_priority_and_threshold);
    RUN_TEST(platform_maps_uart_to_source_two_and_transport_to_source_one);
    RUN_TEST(platform_never_raises_meip_without_enable_and_priority);
    return UNITY_END();
}
PROBE

cat >> "$tree/CMakeLists.txt" <<'CMAKE'

# Appended by tests/guest/run_device_mutation.sh in its throw-away work copy.
if(BUILD_TESTING)
    add_executable(test_device_probe tests/test_device_mutation_probe.c)
    target_link_libraries(test_device_probe PRIVATE yan_machine unity::framework)
    target_compile_options(test_device_probe PRIVATE ${YAN_WARNINGS})
    add_test(NAME device_probe COMMAND test_device_probe)
    set_tests_properties(device_probe PROPERTIES TIMEOUT 10)
endif()
CMAKE

# ------------------------------------------------------------- build/run glue

sanitizer_flag=""
[ "$sanitizers" -eq 1 ] && sanitizer_flag="-DYAN_ENABLE_SANITIZERS=ON"

# Every inner CTest run goes through here: the exclusion list keeps the check
# from recursing into itself, and -j keeps the end-to-end suites affordable on
# a run that repeats the suite once per mutant.
ctest_timeout="${YAN_DEVICE_MUTATION_CTEST_TIMEOUT:-900}"

run_ctest() {
    local log="$1"
    shift
    ( cd "$build" && timeout "$ctest_timeout" ctest --output-on-failure \
        -E "$exclude" -j "$jobs" "$@" ) > "$log" 2>&1
}

configure_tree() {
    # YAN_BUILD_TOOLS adds the Guest end-to-end suites (guest_trap_env,
    # guest_terminal, guest_console) to the oracle. They are off by default:
    # they exercise the console driver rather than the device, and their own
    # mutation harness runs longer than the rest of the suite. With --tools they
    # participate, and excluding device_mutation becomes mandatory because the
    # work copy then registers this very test.
    # shellcheck disable=SC2086
    cmake -S "$tree" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
        -DYAN_BUILD_TOOLS=$tools_flag $sanitizer_flag $unity_arg \
        > "$run/configure.log" 2>&1
}

build_tree() {
    cmake --build "$build" -j "$jobs" > "$run/build.log" 2>&1
}

# The four translation units this script mutates. Their objects are deleted
# before every build so that a rebuild can never be skipped on a timestamp
# comparison: the md5 check proves the source file changed, and this proves the
# compiler read the changed file.
MUTATED_SOURCES="uart transport machine interrupt"

drop_mutated_objects() {
    local name
    for name in $MUTATED_SOURCES; do
        rm -f "$build/CMakeFiles/yan_machine.dir/src/$name.c.o"
    done
}

if ! configure_tree; then
    echo "SKIP: cannot configure the work copy: $(tail -n 1 "$run/configure.log")"
    exit 77
fi
if ! build_tree; then
    echo "SKIP: cannot build the work copy:"
    grep -E 'error|Error' "$run/build.log" | head -n 5
    exit 77
fi

# The probe and the standing suites must be green before any mutation is
# planted, otherwise a failure afterwards proves nothing.
if ! run_ctest "$run/baseline.log"; then
    echo "SKIP: the unmutated work copy does not pass its own suite:"
    sed -n '/The following tests FAILED/,$p' "$run/baseline.log" | head -n 12
    echo "      an unrelated in-flight failure can be excluded with --ignore REGEX"
    exit 77
fi

# ---------------------------------------------------------------- mutations

# One literal pattern per mutation, replaced exactly once (see the count check
# in the python below). A pattern that does not match aborts the branch, so a
# stale text can never be mistaken for a survivor.
apply_mutation() {
    python3 - "$1" "$tree/src/uart.c" "$tree/src/transport.c" "$tree/src/machine.c" \
        "$tree/src/interrupt.c" <<'PY'
import sys

name, uart, transport, machine, interrupt = sys.argv[1:6]

MUTATIONS = {
    "uart-tx-ready-always-on": (uart,
        "*value = (uart_tx_ready(uart) ? YAN_UART_STATUS_TX_READY : 0) |",
        "*value = (uart->terminal_attached ? YAN_UART_STATUS_TX_READY : 0) |"),
    "uart-tx-ready-always-off": (uart,
        "*value = (uart_tx_ready(uart) ? YAN_UART_STATUS_TX_READY : 0) |",
        "*value = 0 |"),
    "uart-tx-write-ignores-readiness": (uart,
        "if (!uart_tx_ready(uart)) {",
        "if (false) {"),
    "uart-disconnect-keeps-the-buffer": (uart,
        """        uart->rx_data = 0;
        uart->rx_available = false;
        uart->irq_status = 0;
        return YAN_OK;""",
        """        uart->irq_status = 0;
        return YAN_OK;"""),
    "uart-rx-ready-ignores-connection": (uart,
        "return uart->terminal_attached && uart->rx_available;",
        "return uart->rx_available;"),
    "uart-rxdata-read-does-not-clear": (uart,
        """            *value = uart->rx_data;
            uart->rx_available = false;""",
        """            *value = uart->rx_data;"""),
    "uart-push-rx-overwrites-a-full-buffer": (uart,
        "if (uart->rx_available) {",
        "if (false) {"),
    "uart-rx-irq-enable-inverted": (uart,
        "(uart->control & YAN_UART_CONTROL_RX_IRQ_ENABLE) != 0;",
        "(uart->control & YAN_UART_CONTROL_RX_IRQ_ENABLE) == 0;"),
    "uart-accepts-half-a-backend": (uart,
        "if (terminal->tx_ready == NULL || terminal->tx_write == NULL) {",
        "if (false) {"),
    "uart-reset-drops-the-backend": (uart,
        """    uart->terminal = terminal;
    uart->terminal_attached = attached;""",
        """    uart->terminal_attached = false;
    (void)terminal;
    (void)attached;"""),
    "transport-capacity-off-by-one": (transport,
        "return size - 1 - ring_used(head, tail, size);",
        "return size - ring_used(head, tail, size);"),
    "transport-doorbell-judges-overflow": (transport,
        "if (transport->notify != NULL) {",
        "if (ring_free(transport->g2h_head, transport->g2h_tail, transport->ring_size) == 0) { transport->overflow_detected = 1; }\n        if (transport->notify != NULL) {"),
    "transport-unavailable-becomes-invalid-state": (transport,
        "return YAN_UNAVAILABLE;",
        "return YAN_INVALID_STATE;"),
    "transport-overrun-moves-the-pointer": (transport,
        """         * the error. */
        transport->overflow_detected = 1;
        return YAN_INVALID_STATE;
    }
    transport->g2h_tail =""",
        """         * the error. */
        transport->overflow_detected = 1;
        transport->g2h_tail = (transport->g2h_tail + bytes) % transport->ring_size;
        return YAN_INVALID_STATE;
    }
    transport->g2h_tail ="""),
    "transport-publish-zero-raises-the-line": (transport,
        "if (bytes == 0) {",
        "if (false) {"),
    "transport-pointers-compared-without-modulo": (transport,
        "return (head - tail) % size;",
        "(void)size;\n    return head - tail;"),
    "transport-read-only-writes-not-ignored": (transport,
        """    case YAN_TRANSPORT_G2H_TAIL:
    case YAN_TRANSPORT_H2G_HEAD:
        /* Constant or host driven, like the hardware-driven bits of mip: a
         * software write is accepted as a no-op and changes nothing. */
        return YAN_OK;""",
        """    case YAN_TRANSPORT_G2H_TAIL:
    case YAN_TRANSPORT_H2G_HEAD:
        transport->g2h_tail = value;
        return YAN_OK;"""),
    "transport-doorbell-notifies-nobody": (transport,
        "transport->notify(transport->notify_context);",
        "(void)0;"),
    "transport-overrun-is-not-latched": (transport,
        "transport->overflow_detected = 1;\n",
        ""),
    "transport-empty-uses-the-full-condition": (transport,
        "ring_used(transport->h2g_head, transport->h2g_tail,",
        "ring_free(transport->h2g_head, transport->h2g_tail,"),
    "transport-host-ready-without-a-callback": (transport,
        "transport->notify != NULL;",
        "transport->ring_size != 0;"),
    "platform-samples-only-the-asserting-direction": (machine,
        """    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_UART,
                       yan_uart_pending(&machine->uart));
    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                       yan_transport_pending(&machine->transport));""",
        """    if (yan_uart_pending(&machine->uart)) {
        yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_UART, true);
    }
    if (yan_transport_pending(&machine->transport)) {
        yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT, true);
    }"""),
    "platform-swaps-the-two-sources": (machine,
        """    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_UART,
                       yan_uart_pending(&machine->uart));
    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                       yan_transport_pending(&machine->transport));""",
        """    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                       yan_uart_pending(&machine->uart));
    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_UART,
                       yan_transport_pending(&machine->transport));"""),
    "plic-complete-does-not-resample-the-level": (interrupt,
        "if ((plic->level & bit) != 0) {",
        "if (false) {"),
    "plic-level-repends-while-in-service": (interrupt,
        "if ((plic->in_service_m & bit) == 0) {",
        "if (true) {"),
    "plic-deassert-keeps-pending": (interrupt,
        "plic->pending &= ~bit;\n",
        ""),
    "plic-claim-ignores-enable": (interrupt,
        "const uint32_t active = plic->pending & plic->enable_m & ~UINT32_C(1);",
        "const uint32_t active = plic->pending & ~UINT32_C(1);"),
    "plic-threshold-ignored": (interrupt,
        "uint32_t selected_priority = plic->threshold_m;",
        "uint32_t selected_priority = 0;"),
}

if name not in MUTATIONS:
    sys.exit("unknown mutation %s" % name)
path, old, new = MUTATIONS[name]
# The unavailable-path mutation changes both host entry points.
expected = 2 if name in ("transport-unavailable-becomes-invalid-state",
                         "transport-overrun-is-not-latched") else 1
text = open(path, encoding="utf-8").read()
count = text.count(old)
if count != expected:
    sys.exit("pattern occurs %d times, expected %d" % (count, expected))
open(path, "w", encoding="utf-8").write(text.replace(old, new))
PY
}

# -------------------------------------------------------------- the run loop

mutations_uart="uart-tx-ready-always-on uart-tx-ready-always-off
uart-tx-write-ignores-readiness
uart-disconnect-keeps-the-buffer uart-rx-ready-ignores-connection
uart-rxdata-read-does-not-clear uart-push-rx-overwrites-a-full-buffer
uart-rx-irq-enable-inverted uart-accepts-half-a-backend uart-reset-drops-the-backend"
mutations_transport="transport-capacity-off-by-one transport-doorbell-judges-overflow
transport-unavailable-becomes-invalid-state transport-overrun-moves-the-pointer
transport-publish-zero-raises-the-line transport-pointers-compared-without-modulo
transport-host-ready-without-a-callback transport-read-only-writes-not-ignored
transport-doorbell-notifies-nobody transport-overrun-is-not-latched
transport-empty-uses-the-full-condition"
mutations_platform="platform-samples-only-the-asserting-direction
platform-swaps-the-two-sources"
mutations_plic="plic-complete-does-not-resample-the-level
plic-level-repends-while-in-service plic-deassert-keeps-pending
plic-claim-ignores-enable plic-threshold-ignored"

case "$target" in
    uart) list="$mutations_uart" ;;
    transport) list="$mutations_transport" ;;
    platform) list="$mutations_platform" ;;
    plic) list="$mutations_plic" ;;
    all) list="$mutations_uart $mutations_transport $mutations_platform $mutations_plic" ;;
esac

detected=0
survivors=0
survivor_names=""
nonassert_names=""
total=0

# The copy is made without -a on purpose: preserving the pristine mtime would
# leave it older than the mutant's object files, and make would then skip the
# rebuild and keep testing the mutant.
restore_sources() {
    for file in uart.c transport.c machine.c interrupt.c; do
        cp "$run/pristine/$file" "$tree/src/$file"
        touch "$tree/src/$file"
    done
}

# A failing test is only evidence when it failed on an assertion: Unity prints
# ":FAIL:" for those. Anything else - crash, timeout, sanitizer report - is
# reported separately and does not count as detection.
failure_kind() {
    # CTest names the suites after the capability ("uart") while the binaries
    # carry the test_ prefix ("test_uart"); script tests such as guest_trap_env
    # have no binary at all and are re-run through CTest.
    # stdbuf keeps the assertion lines that precede a crash: a mutant that
    # fails an assertion and then segfaults would otherwise lose its buffered
    # output and look like a pure crash. The crash is still reported below.
    local name="$1" out="$run/out-$1.log"
    local unbuffered=""
    command -v stdbuf >/dev/null 2>&1 && unbuffered="stdbuf -o0 -e0"
    # stdbuf uses LD_PRELOAD, which makes ASan refuse to start unless the link
    # order check is turned off. Keep the check off only for that combination.
    local asan_env=""
    [ "$sanitizers" -eq 1 ] && asan_env="ASAN_OPTIONS=verify_asan_link_order=0:${ASAN_OPTIONS:-}"
    if [ -x "$build/test_$name" ]; then
        # shellcheck disable=SC2086
        ( cd "$build" && env $asan_env timeout 120 $unbuffered "./test_$name" ) > "$out" 2>&1
    elif [ -x "$build/$name" ]; then
        # shellcheck disable=SC2086
        ( cd "$build" && env $asan_env timeout 120 $unbuffered "./$name" ) > "$out" 2>&1
    else
        run_ctest "$out" -R "^${name}\$"
    fi
    if grep -q ':FAIL:' "$out" 2>/dev/null; then
        echo "assertion"
    else
        echo "other"
    fi
}

for name in $list; do
    total=$((total + 1))
    restore_sources
    before_src="$(cat "$tree/src/uart.c" "$tree/src/transport.c" "$tree/src/machine.c" "$tree/src/interrupt.c" | md5sum)"
    if ! mutate_message="$(apply_mutation "$name" 2>&1)"; then
        echo "FAIL mutant $name was NOT applied (pattern mismatch): $mutate_message"
        survivors=$((survivors + 1)); survivor_names="$survivor_names $name"
        continue
    fi
    after_src="$(cat "$tree/src/uart.c" "$tree/src/transport.c" "$tree/src/machine.c" "$tree/src/interrupt.c" | md5sum)"
    if [ "$before_src" = "$after_src" ]; then
        echo "FAIL mutant $name left the source unchanged"
        survivors=$((survivors + 1)); survivor_names="$survivor_names $name"
        continue
    fi

    drop_mutated_objects
    if ! build_tree; then
        echo "FAIL mutant $name did not build (not an assertion failure):"
        grep -E ' error|Error' "$run/build.log" | head -n 3 | sed 's/^/    /'
        survivors=$((survivors + 1)); survivor_names="$survivor_names $name"
        nonassert_names="$nonassert_names $name"
        continue
    fi

    run_ctest "$run/mutant-$name.log"
    suite_status=$?
    if [ "$suite_status" -eq 0 ]; then
        echo "SURVIVED mutant $name: the whole suite still passed"
        survivors=$((survivors + 1)); survivor_names="$survivor_names $name"
        continue
    fi

    # A mutation can fail a test in three ways: an assertion (***Failed), a
    # crash (***Exception) or a hang (***Timeout). All three are collected here
    # and then classified by failure_kind(), because only the first counts.
    failed="$(awk '
        /Test #[0-9]+: / && /\*\*\*(Failed|Exception|Timeout)/ {
            line = $0
            if (match(line, /Test #[0-9]+: [^ ]+/)) {
                token = substr(line, RSTART, RLENGTH)
                sub(/.*: /, "", token)
                print token
            }
        }' "$run/mutant-$name.log" | sort -u)"

    hits=""
    pseudo=""
    first_assert=""
    for test_name in $failed; do
        if [ "$(failure_kind "$test_name")" = "assertion" ]; then
            hits="$hits $test_name"
            [ -z "$first_assert" ] && \
                first_assert="$(grep -m 1 ':FAIL:' "$run/out-$test_name.log")"
        else
            pseudo="$pseudo $test_name"
        fi
    done

    if [ -n "$hits" ]; then
        echo "PASS mutant $name detected by assertions in:$hits"
        [ -n "$first_assert" ] && echo "    $first_assert"
        detected=$((detected + 1))
    else
        echo "SURVIVED mutant $name: no test failed on an assertion (ctest exit $suite_status)"
        survivors=$((survivors + 1)); survivor_names="$survivor_names $name"
    fi
    if [ -n "$pseudo" ]; then
        echo "    NOTE non-assertion failures in:$pseudo (crash/sanitizer/timeout - not counted)"
        for pseudo_name in $pseudo; do
            [ -s "$run/out-$pseudo_name.log" ] && \
                echo "      $pseudo_name: $(tail -n 1 "$run/out-$pseudo_name.log")"
        done
        nonassert_names="$nonassert_names $name"
    fi
done

# Every source must be back at the revision it started from, and the restored
# copy must pass its own suite again.
restore_sources
for file in uart.c transport.c machine.c interrupt.c; do
    if [ "$(md5sum < "$tree/src/$file")" != "$(md5sum < "$run/pristine/$file")" ]; then
        echo "FAIL the work copy was not restored: src/$file differs from the original"
        survivors=$((survivors + 1))
    fi
done
bash_restore_fail=0
if ! cmake --build "$build" -j "$jobs" --clean-first > "$run/build.log" 2>&1; then
    echo "FAIL the restored work copy does not build again"
    bash_restore_fail=1
elif ! run_ctest "$run/restored.log"; then
    echo "FAIL the restored work copy does not pass its own suite (see $run/restored.log)"
    bash_restore_fail=1
fi

if [ "$bash_restore_fail" -ne 0 ]; then
    echo "BASELINE-RESTORE FAILURE: the restored work copy is not green, so this\n  run proves nothing about the mutations it reported above"
    survivors=$((survivors + 1))
fi

echo "device mutation: $detected detected, $survivors survived, $total mutants"
if [ "$survivors" -ne 0 ]; then
    if [ -n "$survivor_names" ]; then
        echo "SURVIVORS (each needs a new test, not a rerun):$survivor_names"
    fi
    if [ -n "$nonassert_names" ]; then
        echo "  these mutants also had a failure that was NOT an assertion"
        echo "  (crash, timeout or sanitizer report - never counted as detection):"
        echo "   $nonassert_names"
    fi
    exit 1
fi
if [ "$keep" -eq 1 ]; then
    echo "  work tree kept: $tree"
    echo "  run directory : $run"
else
    rm -rf "$tree"
    echo "  work tree removed (use --keep to inspect it); logs: $run/mutant-*.log"
fi
exit 0
