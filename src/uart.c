#include "yan/uart.h"

#include <stddef.h>

/* UART character device. The interface in include/yan/uart.h is frozen by
 * docs/specs/0015-uart-device.md (v3); the register and connection behaviour
 * below follows that specification. Both devices in this window answer 32-bit
 * word accesses only, so a read or write that reaches this file is already a
 * word access; the offset is still checked for alignment because the Bus is not
 * the only caller a unit test can use. */
#define YAN_UART_ALIGNMENT UINT32_C(4)

/* The single readiness predicate. STATUS.TX_READY reports it and the TXDATA
 * write path applies it, so "the device said ready" and "the write is accepted"
 * cannot disagree: TX_READY is a promise, not an advertisement. */
static bool uart_tx_ready(const YanUart *uart)
{
    return uart->terminal_attached &&
           uart->terminal.tx_ready(uart->terminal.context);
}

/* A disconnected device reports no data even if a stale byte were still in the
 * buffer, so RX_READY is not simply rx_available. */
static bool uart_rx_ready(const YanUart *uart)
{
    return uart->terminal_attached && uart->rx_available;
}

void yan_uart_reset(YanUart *uart)
{
    if (uart == NULL) {
        return;
    }
    /* The terminal attachment is host configuration, not run state: a machine
     * reset must not silently detach it. */
    YanUartTerminal terminal = uart->terminal;
    bool attached = uart->terminal_attached;
    *uart = (YanUart){0};
    uart->terminal = terminal;
    uart->terminal_attached = attached;
}

YanStatus yan_uart_read(YanUart *uart, uint32_t offset, uint32_t *value)
{
    if (uart == NULL || value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_UART_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    switch (offset) {
    case YAN_UART_TXDATA:
        /* Write-only: a read returns zero and never touches the transmitter. */
        *value = 0;
        return YAN_OK;
    case YAN_UART_RXDATA:
        /* Connected with a byte waiting: hand it over and clear RX_READY.
         * Otherwise report zero and leave every bit alone, so "a zero byte
         * arrived" and "nothing arrived" stay distinguishable. */
        if (uart_rx_ready(uart)) {
            *value = uart->rx_data;
            uart->rx_available = false;
        } else {
            *value = 0;
        }
        return YAN_OK;
    case YAN_UART_STATUS:
        *value = (uart_tx_ready(uart) ? YAN_UART_STATUS_TX_READY : 0) |
                 (uart_rx_ready(uart) ? YAN_UART_STATUS_RX_READY : 0) |
                 (uart->terminal_attached ? YAN_UART_STATUS_CONNECTED : 0);
        return YAN_OK;
    case YAN_UART_CONTROL:
        *value = uart->control & YAN_UART_CONTROL_RX_IRQ_ENABLE;
        return YAN_OK;
    case YAN_UART_IRQ_STATUS:
        *value = uart->irq_status & YAN_UART_IRQ_RX_PENDING;
        return YAN_OK;
    default:
        return YAN_UNMAPPED;
    }
}

YanStatus yan_uart_write(YanUart *uart, uint32_t offset, uint32_t value)
{
    if (uart == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_UART_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    switch (offset) {
    case YAN_UART_TXDATA:
        /* The same predicate STATUS.TX_READY reports. A byte that cannot be
         * accepted is refused with YAN_UNAVAILABLE: the caller did not write
         * anything wrong, the transmitter is simply not available right now.
         * Dropping it instead would make the output look right while its
         * content is missing. */
        if (!uart_tx_ready(uart)) {
            return YAN_UNAVAILABLE;
        }
        uart->terminal.tx_write(uart->terminal.context,
                                (uint8_t)(value & UINT32_C(0xff)));
        return YAN_OK;
    case YAN_UART_RXDATA:
    case YAN_UART_STATUS:
        /* Read-only registers ignore writes instead of reporting a fault. */
        return YAN_OK;
    case YAN_UART_CONTROL:
        uart->control = value & YAN_UART_CONTROL_RX_IRQ_ENABLE;
        return YAN_OK;
    case YAN_UART_IRQ_STATUS:
        /* Write one to clear: writing zero leaves the flag armed. */
        if ((value & YAN_UART_IRQ_RX_PENDING) != 0) {
            uart->irq_status &= ~YAN_UART_IRQ_RX_PENDING;
        }
        return YAN_OK;
    default:
        return YAN_UNMAPPED;
    }
}

YanStatus yan_uart_set_terminal(YanUart *uart, const YanUartTerminal *terminal)
{
    if (uart == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (terminal == NULL) {
        /* The terminal leaves and takes the unread input and its arrival latch
         * with it. CONTROL is guest state, not terminal state, so it stays. */
        uart->terminal = (YanUartTerminal){0};
        uart->terminal_attached = false;
        uart->rx_data = 0;
        uart->rx_available = false;
        uart->irq_status = 0;
        return YAN_OK;
    }
    if (terminal->tx_ready == NULL || terminal->tx_write == NULL) {
        /* Half a backend cannot carry a byte in both directions; keep the
         * previous attachment instead of replacing it with a broken one. */
        return YAN_INVALID_ARGUMENT;
    }
    /* Copied by value: the caller keeps ownership of its own storage. */
    uart->terminal = *terminal;
    uart->terminal_attached = true;
    return YAN_OK;
}

bool yan_uart_connected(const YanUart *uart)
{
    return uart != NULL && uart->terminal_attached;
}

bool yan_uart_tx_ready(const YanUart *uart)
{
    return uart != NULL && uart_tx_ready(uart);
}

YanStatus yan_uart_push_rx(YanUart *uart, uint8_t byte)
{
    if (uart == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (!uart->terminal_attached) {
        /* No terminal means no other end to receive from: unavailable, not a
         * caller error. */
        return YAN_UNAVAILABLE;
    }
    if (uart->rx_available) {
        /* Single-byte buffer: the new byte is refused, not queued and not
         * overwriting the one the guest has not read yet. A multi-byte FIFO is
         * a decision for a real requirement, such as pasting a line. */
        return YAN_INVALID_STATE;
    }
    uart->rx_data = byte;
    uart->rx_available = true;
    if ((uart->control & YAN_UART_CONTROL_RX_IRQ_ENABLE) != 0) {
        /* Arrival latch for software. The interrupt line itself is the level
         * computed in yan_uart_pending, so it does not depend on this bit. */
        uart->irq_status |= YAN_UART_IRQ_RX_PENDING;
    }
    return YAN_OK;
}

bool yan_uart_pending(const YanUart *uart)
{
    if (uart == NULL) {
        return false;
    }
    /* The line is a level and nothing more: asserted exactly while the device
     * is requesting service, so reading the byte away, clearing RX_IRQ_ENABLE
     * or detaching the terminal withdraws it with no extra bookkeeping.
     * Mapping the line to an interrupt source belongs to the machine, not to
     * this device. */
    return uart_rx_ready(uart) &&
           (uart->control & YAN_UART_CONTROL_RX_IRQ_ENABLE) != 0;
}
