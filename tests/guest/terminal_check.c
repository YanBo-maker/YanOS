/* Guest side of the UART terminal smoke test.
 *
 * Two programs are built from this file and selected at compile time:
 *
 *   default          the transmit/connection check. With a Host terminal
 *                    backend the three bytes of "OK\n" must reach standard
 *                    output byte for byte; with none the program takes the
 *                    CONNECTED=0 path of docs/specs/0015-uart-device.md and
 *                    reports the missing device instead of waiting forever.
 *   TERMINAL_CHECK_MEIP
 *                    the receive-interrupt check. The platform maps the UART
 *                    receive line to PLIC source 2, so a byte the Host pushes
 *                    must arrive as an external interrupt (MEIP): the handler
 *                    claims the source, reads RXDATA and completes, and the
 *                    program echoes what it received. This is the case that can
 *                    only pass while the executor samples the device lines
 *                    before every step -- without that sampling the PLIC never
 *                    sees the line and the bounded wait below expires.
 *
 * Both outcomes are reported through `tohost`, so the runner's exit status says
 * which path broke without anyone parsing the output.
 *
 * The UART and PLIC-source constants are restated here instead of taken from
 * guest_devices.h, which currently describes the CLINT and the PLIC registers
 * but no device window. The style is the same direct 32-bit word access: the
 * devices answer word accesses only, so these accessors must never become byte
 * or halfword operations.
 */
#include "guest.h"
#include "guest_devices.h"

#ifdef TERMINAL_CHECK_MEIP
#include "mtrap.h"
#endif

#define YAN_GUEST_UART_BASE UINT32_C(0x10000000)
#define YAN_GUEST_UART_TXDATA UINT32_C(0x00000000)
#define YAN_GUEST_UART_RXDATA UINT32_C(0x00000004)
#define YAN_GUEST_UART_STATUS UINT32_C(0x00000008)
#define YAN_GUEST_UART_CONTROL UINT32_C(0x0000000c)

#define YAN_GUEST_UART_STATUS_TX_READY UINT32_C(0x00000001)
#define YAN_GUEST_UART_STATUS_CONNECTED UINT32_C(0x00000004)
#define YAN_GUEST_UART_CONTROL_RX_IRQ_ENABLE UINT32_C(0x00000001)

/* Platform wiring rather than a device property: include/yan/machine.h assigns
 * the UART receive line to PLIC source 2. guest_devices.h carries no source
 * table, so the number is restated together with its origin. */
#define YAN_GUEST_PLIC_SOURCE_UART UINT32_C(2)

/* The driver contract allows waiting for TX_READY only while a limit is
 * enforced. Each attempt is a handful of instructions, so this stays far below
 * any sane step budget while still being long enough to ride out a backend that
 * is briefly busy. */
#define TX_POLL_LIMIT UINT32_C(100000)
/* Same reasoning for the interrupt: long enough that a working platform answers
 * within the first few instructions, short enough that a Guest which never
 * terminates is impossible. */
#define MEIP_WAIT_LIMIT UINT32_C(200000)

/* Check codes reported through `tohost`; 1 means the program reached the end.
 * The runner turns any other value into its Guest-failure exit code and prints
 * the number, so a failure says which path broke. */
#define CHECK_NO_TERMINAL UINT32_C(10)
#define CHECK_TX_READY UINT32_C(11)
#define CHECK_MEIP UINT32_C(21)
#define CHECK_MEIP_BYTE UINT32_C(22)

/* The byte run_terminal.sh feeds on standard input for the interrupt check.
 * Both sides name it, so a mismatch is a broken copy rather than a race. */
#define TERMINAL_CHECK_BYTE ((uint8_t)'Q')

static uint32_t uart_status(void)
{
    return yan_guest_mmio_read32(YAN_GUEST_UART_BASE + YAN_GUEST_UART_STATUS);
}

/* False when TX_READY stayed zero for the whole retry budget. */
static int wait_tx_ready(void)
{
    for (uint32_t attempt = 0; attempt < TX_POLL_LIMIT; ++attempt) {
        if ((uart_status() & YAN_GUEST_UART_STATUS_TX_READY) != 0) {
            return 1;
        }
    }
    return 0;
}

/* Writes one byte at a time, re-reading the ready bit before each: a previous
 * byte may have consumed the backend's room. TX_READY is a promise, so once it
 * reads 1 the write is accepted and the Guest has no way to observe a refusal
 * (a refused TXDATA leaves no status behind). */
static int write_bytes(const uint8_t *bytes, size_t count)
{
    for (size_t at = 0; at < count; ++at) {
        if (!wait_tx_ready()) {
            return 0;
        }
        yan_guest_mmio_write32(YAN_GUEST_UART_BASE + YAN_GUEST_UART_TXDATA,
                               (uint32_t)bytes[at]);
    }
    return 1;
}

#ifdef TERMINAL_CHECK_MEIP
/* Written by the handler and read by the wait loop, so both are volatile: the
 * loop is the program's only proof that the interrupt arrived. */
static volatile uint32_t meip_count;
static volatile uint32_t received;

static uint32_t uart_rx_handler(uint32_t cause, uint32_t epc, uint32_t tval)
{
    (void)tval;
    if (cause != (YAN_GUEST_CAUSE_INTERRUPT | YAN_GUEST_CAUSE_MEIP)) {
        /* Only MEIP is enabled, so this would mean the CSR state is not the one
         * this program configured. Returning epc lets the wait loop expire and
         * report that instead of resuming somewhere unrelated. */
        return epc;
    }
    const uint32_t source = yan_guest_plic_claim();
    if (source == YAN_GUEST_PLIC_SOURCE_UART) {
        received = yan_guest_mmio_read32(YAN_GUEST_UART_BASE +
                                         YAN_GUEST_UART_RXDATA) &
                   UINT32_C(0xff);
        ++meip_count;
    }
    if (source != 0) {
        /* Completion re-samples the line; the byte has been read away, so the
         * device stops asking and the interrupt is not pended again. */
        yan_guest_plic_complete(source);
    }
    /* An interrupt has not executed anything yet: resume the instruction the
     * handler interrupted. */
    return epc;
}

int main(void)
{
    if ((uart_status() & YAN_GUEST_UART_STATUS_CONNECTED) == 0) {
        guest_finish(CHECK_NO_TERMINAL);
    }

    yan_guest_trap_install(uart_rx_handler);
    /* Route the receive line to MEIP: the PLIC needs a priority above its
     * threshold plus the enable bit before the line can reach the CPU. */
    yan_guest_plic_set_priority(YAN_GUEST_PLIC_SOURCE_UART, 1);
    yan_guest_plic_set_enable(UINT32_C(1) << YAN_GUEST_PLIC_SOURCE_UART);
    yan_guest_plic_set_threshold(0);
    yan_guest_mmio_write32(YAN_GUEST_UART_BASE + YAN_GUEST_UART_CONTROL,
                           YAN_GUEST_UART_CONTROL_RX_IRQ_ENABLE);
    yan_guest_enable_interrupts(YAN_GUEST_MIE_MEIP);

    for (uint32_t attempt = 0; attempt < MEIP_WAIT_LIMIT && meip_count == 0;
         ++attempt) {
    }
    yan_guest_disable_interrupts();

    if (meip_count == 0) {
        guest_finish(CHECK_MEIP);
    }
    if (received != TERMINAL_CHECK_BYTE) {
        guest_finish(CHECK_MEIP_BYTE);
    }
    const uint8_t echo[2] = {(uint8_t)received, (uint8_t)'\n'};
    if (!write_bytes(echo, sizeof(echo))) {
        guest_finish(CHECK_TX_READY);
    }
    guest_finish(1);
    return 0;
}
#else
int main(void)
{
    if ((uart_status() & YAN_GUEST_UART_STATUS_CONNECTED) == 0) {
        guest_finish(CHECK_NO_TERMINAL);
    }

    static const uint8_t line[] = "OK\n";
    if (!write_bytes(line, sizeof(line) - 1)) {
        guest_finish(CHECK_TX_READY);
    }
    guest_finish(1);
    return 0;
}
#endif
