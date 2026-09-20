#include <string.h>

#include "yan/bus.h"
#include "yan/machine.h"
#include "yan/uart.h"
#include "unity.h"

/* The UART cases are derived from docs/specs/0015-uart-device.md (v3). Most of
 * them drive the device through the Bus, the same path a guest takes; the
 * Machine cases at the end cover the platform wiring (whole-machine reset, PLIC
 * source 2, two independent machines).
 *
 * The v3 revision makes connection state and transmit readiness first-class, so
 * three groups are named explicitly:
 *   - disconnected_uart_is_inert_and_reports_unavailable
 *   - connected_uart_reports_and_transmits
 *   - tx_write_rejected_after_ready_was_sampled
 * Together they pin the invariant that STATUS.TX_READY and the TXDATA write
 * path answer the same question. */

#define TERMINAL_MAGIC UINT32_C(0x5445524d)
#define TX_CAPTURE_CAPACITY 16

/* Host terminal backend used by the tests: it reports readiness on demand and
 * captures every byte the device hands over. The magic field checks that the
 * device passes back the context it was given. */
typedef struct {
    uint32_t magic;
    bool ready;
    uint8_t bytes[TX_CAPTURE_CAPACITY];
    size_t count;
} TestTerminal;

static YanRam ram;
static YanBus bus;
static YanUart uart;

static YanMachine machine;
static YanMachine other;

static TestTerminal terminal;
static TestTerminal other_terminal;

static void reset_terminal(TestTerminal *backend)
{
    *backend = (TestTerminal){.magic = TERMINAL_MAGIC, .ready = true};
}

static bool backend_ready(void *context)
{
    TestTerminal *self = context;
    TEST_ASSERT_TRUE(self->magic == TERMINAL_MAGIC);
    return self->ready;
}

static void backend_write(void *context, uint8_t byte)
{
    TestTerminal *self = context;
    TEST_ASSERT_TRUE(self->magic == TERMINAL_MAGIC);
    /* The device promises acceptance before advertising TX_READY, so it must
     * never call the sink while the backend reports "not ready". */
    TEST_ASSERT_TRUE(self->ready);
    if (self->count < TX_CAPTURE_CAPACITY) {
        self->bytes[self->count] = byte;
    }
    ++self->count;
}

static void connect_terminal_to(YanUart *target, TestTerminal *backend)
{
    backend->ready = true;
    const YanUartTerminal attachment = {backend, backend_ready, backend_write};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(target, &attachment));
}

#define REG(offset) (YAN_UART_BASE + (offset))

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    uart = (YanUart){0};
    machine = (YanMachine){0};
    other = (YanMachine){0};
    reset_terminal(&terminal);
    reset_terminal(&other_terminal);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 64));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    bus.uart = &uart;
    yan_uart_reset(&uart);
}

void tearDown(void)
{
    yan_machine_destroy(&machine);
    yan_machine_destroy(&other);
    yan_ram_destroy(&ram);
}

static uint32_t read_reg(uint32_t offset)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&bus, REG(offset), 4, &value).status);
    return value;
}

static void write_reg(uint32_t offset, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&bus, REG(offset), 4, value).status);
}

static uint32_t read_machine_word(const YanBus *target, uint32_t address)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(target, address, 4, &value).status);
    return value;
}

static void write_machine_word(YanBus *target, uint32_t address, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(target, address, 4, value).status);
}

static uint32_t plic_word(uint32_t offset)
{
    return read_machine_word(&machine.bus, YAN_PLIC_BASE + offset);
}

static void enable_uart_source(void)
{
    write_machine_word(&machine.bus,
                       YAN_PLIC_BASE + YAN_PLIC_PRIORITY +
                           4U * YAN_MACHINE_PLIC_SOURCE_UART,
                       1);
    write_machine_word(&machine.bus, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M,
                       UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART);
}

/* --------------------------------------------------------------- reset values */

static void reset_values(void)
{
    /* Without a backend every status bit is zero: there is no terminal to be
     * connected to, nothing to transmit to, and nothing to read. */
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_TXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));

    TEST_ASSERT_EQUAL_HEX32(0, uart.control);
    TEST_ASSERT_EQUAL_HEX32(0, uart.irq_status);
    TEST_ASSERT_FALSE(uart.rx_available);
    TEST_ASSERT_FALSE(yan_uart_connected(&uart));
    TEST_ASSERT_FALSE(yan_uart_tx_ready(&uart));
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));

    /* The three documented status bits are exactly the ones that exist. */
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x1), YAN_UART_STATUS_TX_READY);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x2), YAN_UART_STATUS_RX_READY);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x4), YAN_UART_STATUS_CONNECTED);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x1), YAN_UART_IRQ_RX_PENDING);
}

static void reserved_bits_read_zero(void)
{
    connect_terminal_to(&uart, &terminal);

    /* CONTROL keeps bit 0 only, both inside the device and on the way out. */
    write_reg(YAN_UART_CONTROL, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_CONTROL_RX_IRQ_ENABLE, uart.control);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_CONTROL_RX_IRQ_ENABLE,
                            read_reg(YAN_UART_CONTROL));

    /* With a byte waiting, STATUS has all three defined bits set and nothing
     * above them. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x41));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING,
                            read_reg(YAN_UART_IRQ_STATUS));

    /* Writing reserved bits cannot smuggle them into the registers. */
    write_reg(YAN_UART_IRQ_STATUS, UINT32_C(0xfffffffe));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING, uart.irq_status);
    write_reg(YAN_UART_CONTROL, UINT32_C(0xfffffff0));
    TEST_ASSERT_EQUAL_HEX32(0, uart.control);
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_CONTROL));
}

static void read_only_writes_are_ignored(void)
{
    /* Before any attachment: the window answers, the writes do nothing. */
    const uint32_t status = read_reg(YAN_UART_STATUS);
    write_reg(YAN_UART_RXDATA, UINT32_C(0xdeadbeef));
    write_reg(YAN_UART_STATUS, 0);
    write_reg(YAN_UART_STATUS, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(status, read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));

    /* A buffered byte survives the same writes: they neither drop it nor clear
     * RX_READY. */
    connect_terminal_to(&uart, &terminal);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x5a));
    write_reg(YAN_UART_RXDATA, 0);
    write_reg(YAN_UART_STATUS, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0x5a, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_FALSE(uart.rx_available);

    /* TXDATA is write-only: reading it returns zero and never transmits. */
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_TXDATA));
    TEST_ASSERT_EQUAL_size_t(0, terminal.count);
}

/* -------------------------------------------------------------- disconnected */

static void disconnected_uart_is_inert_and_reports_unavailable(void)
{
    /* Group 1: no backend attached. The device must report that honestly
     * instead of advertising a ready bit it cannot honour. */
    TEST_ASSERT_FALSE(yan_uart_connected(&uart));
    TEST_ASSERT_FALSE(yan_uart_tx_ready(&uart));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_STATUS));

    /* Both directions answer YAN_UNAVAILABLE: the caller did nothing wrong, the
     * other end is simply not there. YAN_UNAVAILABLE (not YAN_INVALID_STATE) is
     * what lets a guest driver tell "headless" apart from "I called this
     * wrong". */
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE,
                          yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, 'B').status);
    TEST_ASSERT_EQUAL_size_t(0, terminal.count);
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_uart_push_rx(&uart, 0x5a));
    TEST_ASSERT_FALSE(uart.rx_available);
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_STATUS));

    /* A read of RXDATA is legal and reports "nothing", changing no state. */
    const uint32_t control = read_reg(YAN_UART_CONTROL);
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(control, read_reg(YAN_UART_CONTROL));

    /* Even with receive interrupts enabled the line stays deasserted: a
     * disconnected device has nothing to report. */
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));
}

/* ----------------------------------------------------------------- connected */

static void connected_uart_reports_and_transmits(void)
{
    /* Group 2: a ready backend. CONNECTED and TX_READY are visible, and an
     * accepted write hands over exactly one byte, low 8 bits. */
    connect_terminal_to(&uart, &terminal);
    TEST_ASSERT_TRUE(yan_uart_connected(&uart));
    TEST_ASSERT_TRUE(yan_uart_tx_ready(&uart));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));

    TEST_ASSERT_EQUAL_INT(
        YAN_OK,
        yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, UINT32_C(0x00000141)).status);
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    TEST_ASSERT_EQUAL_HEX8('A', terminal.bytes[0]);

    TEST_ASSERT_EQUAL_INT(
        YAN_OK,
        yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, UINT32_C(0xffffff00)).status);
    TEST_ASSERT_EQUAL_size_t(2, terminal.count);
    TEST_ASSERT_EQUAL_HEX8(0x00, terminal.bytes[1]);

    /* The receive direction works on the same attachment. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x5a));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0x5a, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_FALSE(uart.rx_available);
}

static void tx_write_rejected_after_ready_was_sampled(void)
{
    /* Group 3: the read-to-write window. The guest samples TX_READY=1, the
     * backend stops accepting, and the write that follows must report the
     * refusal instead of dropping the byte. */
    connect_terminal_to(&uart, &terminal);
    const uint32_t sampled = read_reg(YAN_UART_STATUS);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY,
                            sampled & YAN_UART_STATUS_TX_READY);

    terminal.ready = false;
    TEST_ASSERT_FALSE(yan_uart_tx_ready(&uart));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_CONNECTED, read_reg(YAN_UART_STATUS));

    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE,
                          yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, 'A').status);
    TEST_ASSERT_EQUAL_size_t(0, terminal.count);

    /* Ready again: the same write is accepted, and exactly once. */
    terminal.ready = true;
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, 'A').status);
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    TEST_ASSERT_EQUAL_HEX8('A', terminal.bytes[0]);
}

static void ready_bit_matches_write_acceptance(void)
{
    /* The invariant behind TX_READY: whatever the backend reports, the status
     * bit and the write result agree, because both use one predicate. */
    connect_terminal_to(&uart, &terminal);
    for (int pass = 0; pass < 2; ++pass) {
        terminal.ready = (pass == 0);
        terminal.count = 0;
        const bool advertised =
            (read_reg(YAN_UART_STATUS) & YAN_UART_STATUS_TX_READY) != 0;
        const YanStatus status =
            yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, 'x').status;
        TEST_ASSERT_EQUAL_INT(advertised ? YAN_OK : YAN_UNAVAILABLE, status);
        TEST_ASSERT_EQUAL_size_t(advertised ? 1 : 0, terminal.count);
    }
}

/* --------------------------------------------------------- connection lifecycle */

static void terminal_backend_must_have_both_callbacks(void)
{
    const YanUartTerminal no_ready = {&terminal, NULL, backend_write};
    const YanUartTerminal no_write = {&terminal, backend_ready, NULL};
    const YanUartTerminal empty = {NULL, NULL, NULL};

    /* A rejected backend must not replace the existing attachment. */
    connect_terminal_to(&uart, &terminal);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_set_terminal(&uart, &no_ready));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_set_terminal(&uart, &no_write));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_uart_set_terminal(&uart, &empty));
    TEST_ASSERT_TRUE(yan_uart_connected(&uart));
    TEST_ASSERT_TRUE(yan_uart_tx_ready(&uart));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, REG(YAN_UART_TXDATA), 4, 'K').status);
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    TEST_ASSERT_EQUAL_HEX8('K', terminal.bytes[0]);

    /* NULL disconnects; a half-built backend is rejected, and a rejected
     * backend cannot connect a device that has no terminal. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, NULL));
    TEST_ASSERT_FALSE(yan_uart_connected(&uart));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_set_terminal(&uart, &no_write));
    TEST_ASSERT_FALSE(yan_uart_connected(&uart));
    /* Disconnecting again is a no-op, not an error. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, NULL));
}

static void disconnect_clears_receive_state(void)
{
    connect_terminal_to(&uart, &terminal);
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x5a));
    TEST_ASSERT_TRUE(yan_uart_pending(&uart));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING, read_reg(YAN_UART_IRQ_STATUS));

    /* The terminal leaving takes the unread input with it. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&uart, NULL));

    TEST_ASSERT_FALSE(yan_uart_connected(&uart));
    TEST_ASSERT_FALSE(yan_uart_tx_ready(&uart));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));
    TEST_ASSERT_FALSE(uart.rx_available);
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));
    /* CONTROL is guest state, not terminal state, so it survives. */
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_CONTROL_RX_IRQ_ENABLE,
                            read_reg(YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));

    /* Reconnecting finds an empty device, not the byte the old terminal left. */
    connect_terminal_to(&uart, &terminal);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));
}

static void reset_keeps_terminal_attachment(void)
{
    connect_terminal_to(&uart, &terminal);
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x41));

    yan_uart_reset(&uart);

    /* Run state is gone ... */
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    /* ... but the terminal is host configuration and stays attached, context
     * included: the same backend still receives output. */
    TEST_ASSERT_TRUE(yan_uart_connected(&uart));
    TEST_ASSERT_TRUE(yan_uart_tx_ready(&uart));
    write_reg(YAN_UART_TXDATA, 'R');
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    TEST_ASSERT_EQUAL_HEX8('R', terminal.bytes[0]);
}

/* -------------------------------------------------------------------- receive */

static void receive_is_a_single_byte_buffer(void)
{
    connect_terminal_to(&uart, &terminal);

    /* Empty: a read reports zero without arming anything. */
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x5a));
    TEST_ASSERT_TRUE(uart.rx_available);
    TEST_ASSERT_EQUAL_HEX8(0x5a, uart.rx_data);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));

    /* Full: the second byte is refused, not queued and not overwriting. */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_uart_push_rx(&uart, 0x99));
    TEST_ASSERT_EQUAL_HEX8(0x5a, uart.rx_data);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));

    /* The read takes the byte and clears the flag; the buffer is reusable. */
    TEST_ASSERT_EQUAL_HEX32(0x5a, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_FALSE(uart.rx_available);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x99));
    TEST_ASSERT_EQUAL_HEX32(0x99, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
}

static void read_empty_changes_nothing(void)
{
    connect_terminal_to(&uart, &terminal);
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    const uint32_t status = read_reg(YAN_UART_STATUS);
    const uint32_t control = read_reg(YAN_UART_CONTROL);
    const uint32_t irq_status = read_reg(YAN_UART_IRQ_STATUS);
    const bool line = yan_uart_pending(&uart);

    /* Two empty reads in a row: no bit moves. "A zero byte arrived" and
     * "nothing arrived" have to stay distinguishable. */
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(status, read_reg(YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(control, read_reg(YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_HEX32(irq_status, read_reg(YAN_UART_IRQ_STATUS));
    TEST_ASSERT_EQUAL_INT(line, yan_uart_pending(&uart));
    TEST_ASSERT_FALSE(uart.rx_available);
}

static void rx_irq_line_follows_enable_and_data(void)
{
    connect_terminal_to(&uart, &terminal);

    /* Data while the line is disabled must not assert it; the arrival latch is
     * not armed either. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x41));
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));

    /* Enabling while the byte is still buffered asserts it: the line is a
     * level, not an edge that was missed. */
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_TRUE(yan_uart_pending(&uart));

    /* Clearing the enable withdraws it at once, and re-enabling brings it back
     * while the data is still there. */
    write_reg(YAN_UART_CONTROL, 0);
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_TRUE(yan_uart_pending(&uart));

    /* Reading the byte away withdraws it for good. */
    TEST_ASSERT_EQUAL_HEX32(0x41, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_FALSE(yan_uart_pending(&uart));
}

static void irq_status_is_write_one_to_clear(void)
{
    connect_terminal_to(&uart, &terminal);
    write_reg(YAN_UART_CONTROL, YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x7e));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING, read_reg(YAN_UART_IRQ_STATUS));

    /* Writing zero to bit 0 leaves the flag armed; there is no "clear all". */
    write_reg(YAN_UART_IRQ_STATUS, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING, read_reg(YAN_UART_IRQ_STATUS));

    write_reg(YAN_UART_IRQ_STATUS, YAN_UART_IRQ_RX_PENDING);
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));

    /* The latch records an arrival, not the level: the byte is still buffered
     * and the line is still asserted. */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_uart_push_rx(&uart, 0x7f));
    TEST_ASSERT_EQUAL_HEX32(0, read_reg(YAN_UART_IRQ_STATUS));
    TEST_ASSERT_TRUE(yan_uart_pending(&uart));

    /* A refused push has no side effect: the buffered byte is still the old
     * one, and a later push re-arms the flag. */
    TEST_ASSERT_EQUAL_HEX32(0x7e, read_reg(YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&uart, 0x7f));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING, read_reg(YAN_UART_IRQ_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0x7f, read_reg(YAN_UART_RXDATA));
}

/* ----------------------------------------------------- window and error decode */

static void window_bounds_widths_and_absent_device(void)
{
    uint32_t value = 0;

    connect_terminal_to(&uart, &terminal);

    /* Only 4-byte accesses enter the device; narrower ones keep the RAM-only
     * answer and must not touch a register or deliver a byte. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, REG(YAN_UART_TXDATA), 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, REG(YAN_UART_RXDATA), 2, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, REG(YAN_UART_TXDATA), 1, 0x41).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, REG(YAN_UART_CONTROL), 2, UINT32_MAX)
                              .status);
    TEST_ASSERT_EQUAL_size_t(0, terminal.count);
    TEST_ASSERT_EQUAL_HEX32(0, uart.control);

    /* Unaligned word accesses never reach a register either. */
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_read(&bus, REG(YAN_UART_TXDATA) + 2, 4, &value)
                              .status);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_write(&bus, REG(YAN_UART_RXDATA) + 1, 4, 0).status);
    TEST_ASSERT_EQUAL_size_t(0, terminal.count);

    /* Offsets inside the window with no register behind them are unmapped ... */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, REG(YAN_UART_IRQ_STATUS + 4), 4, &value)
                              .status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, REG(YAN_UART_IRQ_STATUS + 4), 4, 0)
                              .status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, REG(YAN_UART_SIZE - 4), 4, &value)
                              .status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, REG(YAN_UART_SIZE - 4), 4, 0).status);

    /* ... and the window ends at YAN_UART_SIZE. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, REG(YAN_UART_SIZE), 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, REG(YAN_UART_SIZE), 4, 0).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_UART_BASE - 4, 4, &value).status);

    /* The MMIO window is a data mapping, not code. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_fetch32(&bus, REG(YAN_UART_TXDATA), &value).status);

    /* A bus with no UART wired leaves the whole window unmapped. */
    YanBus plain = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_init(&plain, &ram, UINT32_C(0x80000000)));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&plain, REG(YAN_UART_STATUS), 4, &value)
                              .status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&plain, REG(YAN_UART_TXDATA), 4, 0x41).status);
}

static void null_and_argument_errors(void)
{
    uint32_t value = UINT32_C(0x12345678);
    const YanUartTerminal attachment = {&terminal, backend_ready, backend_write};

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_read(NULL, YAN_UART_STATUS, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_read(&uart, YAN_UART_STATUS, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_write(NULL, YAN_UART_TXDATA, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_uart_push_rx(NULL, 0x41));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_uart_set_terminal(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_uart_set_terminal(NULL, &attachment));
    TEST_ASSERT_FALSE(yan_uart_pending(NULL));
    TEST_ASSERT_FALSE(yan_uart_connected(NULL));
    TEST_ASSERT_FALSE(yan_uart_tx_ready(NULL));
    yan_uart_reset(NULL);

    /* The device checks alignment for callers that skip the Bus, and a rejected
     * access leaves the output value untouched. */
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_uart_read(&uart, YAN_UART_STATUS + 2, &value));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x12345678), value);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_uart_write(&uart, YAN_UART_TXDATA + 2, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_uart_read(&uart, YAN_UART_IRQ_STATUS + 4, &value));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_uart_write(&uart, YAN_UART_IRQ_STATUS + 4, 0));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x12345678), value);
}

/* ------------------------------------------------------- machine integration */

static void machine_reset_clears_run_state_and_keeps_terminal(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    connect_terminal_to(&machine.uart, &terminal);

    write_machine_word(&machine.bus, YAN_UART_BASE + YAN_UART_CONTROL,
                       YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&machine.uart, 0x5a));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_machine_word(&machine.bus,
                                              YAN_UART_BASE + YAN_UART_STATUS));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&machine));

    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_machine_word(&machine.bus,
                                              YAN_UART_BASE + YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_machine_word(&machine.bus,
                                                 YAN_UART_BASE + YAN_UART_CONTROL));
    TEST_ASSERT_EQUAL_HEX32(0, read_machine_word(&machine.bus,
                                                 YAN_UART_BASE +
                                                     YAN_UART_IRQ_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_machine_word(&machine.bus,
                                                 YAN_UART_BASE + YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);

    /* A reset device with no data must not re-assert the source. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);

    /* The terminal survived the reset: guest output still reaches the host. */
    write_machine_word(&machine.bus, YAN_UART_BASE + YAN_UART_TXDATA, 'R');
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    TEST_ASSERT_EQUAL_HEX8('R', terminal.bytes[0]);
}

static void receive_interrupt_reaches_plic_source_two(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    enable_uart_source();
    connect_terminal_to(&machine.uart, &terminal);

    /* Line disabled: a byte arrives and nothing reaches MEIP. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&machine.uart, 0x41));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));

    /* Enabled: the line drives source 2 and MEIP follows the arbitration. */
    write_machine_word(&machine.bus, YAN_UART_BASE + YAN_UART_CONTROL,
                       YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            plic_word(YAN_PLIC_PENDING));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP,
                            yan_bus_pending_interrupts(&machine.bus));

    /* The handler claims source 2 ... */
    TEST_ASSERT_EQUAL_HEX32(YAN_MACHINE_PLIC_SOURCE_UART,
                            plic_word(YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);

    /* ... completes it, and the next sample raises it again because the byte is
     * still unread: the device line is a level, not a one-shot edge. */
    write_machine_word(&machine.bus, YAN_PLIC_BASE + YAN_PLIC_CLAIM_M,
                       YAN_MACHINE_PLIC_SOURCE_UART);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP,
                            yan_bus_pending_interrupts(&machine.bus));

    /* Reading the byte away withdraws the level, so the claim that follows
     * leaves nothing behind. */
    TEST_ASSERT_EQUAL_HEX32(0x41, read_machine_word(&machine.bus,
                                                    YAN_UART_BASE +
                                                        YAN_UART_RXDATA));
    TEST_ASSERT_EQUAL_HEX32(YAN_MACHINE_PLIC_SOURCE_UART,
                            plic_word(YAN_PLIC_CLAIM_M));
    write_machine_word(&machine.bus, YAN_PLIC_BASE + YAN_PLIC_CLAIM_M,
                       YAN_MACHINE_PLIC_SOURCE_UART);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));

    /* Disconnecting also deasserts the line, so a stale source cannot survive
     * the terminal going away. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&machine.uart, 0x42));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&machine.uart, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));
}

static void machines_do_not_share_uart_state(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&other));
    connect_terminal_to(&machine.uart, &terminal);
    connect_terminal_to(&other.uart, &other_terminal);

    write_machine_word(&machine.bus, YAN_UART_BASE + YAN_UART_CONTROL,
                       YAN_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_push_rx(&machine.uart, 0x5a));

    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_RX_READY |
                                YAN_UART_STATUS_CONNECTED,
                            read_machine_word(&machine.bus,
                                              YAN_UART_BASE + YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY | YAN_UART_STATUS_CONNECTED,
                            read_machine_word(&other.bus,
                                              YAN_UART_BASE + YAN_UART_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, read_machine_word(&other.bus,
                                                 YAN_UART_BASE + YAN_UART_CONTROL));
    TEST_ASSERT_TRUE(yan_uart_pending(&machine.uart));
    TEST_ASSERT_FALSE(yan_uart_pending(&other.uart));

    /* Sampling one machine touches only its own PLIC. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, other.plic.pending);

    /* Each machine has its own terminal: output goes to its own backend, and
     * detaching one leaves the other connected. */
    write_machine_word(&other.bus, YAN_UART_BASE + YAN_UART_TXDATA, 'B');
    write_machine_word(&machine.bus, YAN_UART_BASE + YAN_UART_TXDATA, 'A');
    TEST_ASSERT_EQUAL_size_t(1, other_terminal.count);
    TEST_ASSERT_EQUAL_HEX8('B', other_terminal.bytes[0]);
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    TEST_ASSERT_EQUAL_HEX8('A', terminal.bytes[0]);

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_uart_set_terminal(&machine.uart, NULL));
    TEST_ASSERT_FALSE(yan_uart_connected(&machine.uart));
    TEST_ASSERT_TRUE(yan_uart_connected(&other.uart));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE,
                          yan_bus_write(&machine.bus,
                                        YAN_UART_BASE + YAN_UART_TXDATA, 4, 'C')
                              .status);
    TEST_ASSERT_EQUAL_size_t(1, terminal.count);
    write_machine_word(&other.bus, YAN_UART_BASE + YAN_UART_TXDATA, 'D');
    TEST_ASSERT_EQUAL_size_t(2, other_terminal.count);
    TEST_ASSERT_EQUAL_HEX8('D', other_terminal.bytes[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_values);
    RUN_TEST(reserved_bits_read_zero);
    RUN_TEST(read_only_writes_are_ignored);
    RUN_TEST(disconnected_uart_is_inert_and_reports_unavailable);
    RUN_TEST(connected_uart_reports_and_transmits);
    RUN_TEST(tx_write_rejected_after_ready_was_sampled);
    RUN_TEST(ready_bit_matches_write_acceptance);
    RUN_TEST(terminal_backend_must_have_both_callbacks);
    RUN_TEST(disconnect_clears_receive_state);
    RUN_TEST(reset_keeps_terminal_attachment);
    RUN_TEST(receive_is_a_single_byte_buffer);
    RUN_TEST(read_empty_changes_nothing);
    RUN_TEST(rx_irq_line_follows_enable_and_data);
    RUN_TEST(irq_status_is_write_one_to_clear);
    RUN_TEST(window_bounds_widths_and_absent_device);
    RUN_TEST(null_and_argument_errors);
    RUN_TEST(machine_reset_clears_run_state_and_keeps_terminal);
    RUN_TEST(receive_interrupt_reaches_plic_source_two);
    RUN_TEST(machines_do_not_share_uart_state);
    return UNITY_END();
}
