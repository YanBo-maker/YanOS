#ifndef YAN_UART_H
#define YAN_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "yan/status.h"

/* Minimal character device for the guest console. See
 * docs/specs/0015-uart-device.md; this header is the frozen interface and the
 * implementation lives in src/uart.c. */
#define YAN_UART_BASE UINT32_C(0x10000000)
#define YAN_UART_SIZE UINT32_C(0x00001000)

#define YAN_UART_TXDATA UINT32_C(0x00)
#define YAN_UART_RXDATA UINT32_C(0x04)
#define YAN_UART_STATUS UINT32_C(0x08)
#define YAN_UART_CONTROL UINT32_C(0x0c)
#define YAN_UART_IRQ_STATUS UINT32_C(0x10)

/* STATUS. TX_READY means "writing TXDATA right now would be accepted": the
 * device must never report TX_READY=1 and then refuse the write. Without an
 * attached backend all three bits read 0. */
#define YAN_UART_STATUS_TX_READY UINT32_C(0x00000001)
#define YAN_UART_STATUS_RX_READY UINT32_C(0x00000002)
#define YAN_UART_STATUS_CONNECTED UINT32_C(0x00000004)

#define YAN_UART_CONTROL_RX_IRQ_ENABLE UINT32_C(0x00000001)

/* IRQ_STATUS latches "data arrived while receive interrupts were enabled". It
 * is not the same thing as STATUS.RX_READY, which reports whether data is
 * available now; the bit is named for the interrupt it records. */
#define YAN_UART_IRQ_RX_PENDING UINT32_C(0x00000001)

/* A host terminal backend. Attaching one connects the device; a device without
 * one reports CONNECTED=0 and refuses both directions with YAN_UNAVAILABLE, so
 * a guest driver can run headless instead of polling forever. */
typedef struct {
    void *context;
    /* True when the backend can accept one byte right now. */
    bool (*tx_ready)(void *context);
    /* Accept one byte. The device calls this only after tx_ready() returned
     * true, so an implementation must not refuse. */
    void (*tx_write)(void *context, uint8_t byte);
} YanUartTerminal;

typedef struct {
    uint32_t control;
    uint32_t irq_status;
    /* Single-byte receive buffer: no FIFO in this stage. */
    uint8_t rx_data;
    bool rx_available;
    /* Attached backend, copied by value so the caller keeps ownership of its
     * own storage. terminal_attached is false until a usable backend is set. */
    YanUartTerminal terminal;
    bool terminal_attached;
} YanUart;

/* Reset clears the run state and keeps the terminal attachment, because the
 * attachment is host configuration rather than device state. */
void yan_uart_reset(YanUart *uart);
/* A read of RXDATA clears RX_READY, so the accessor takes a mutable device,
 * matching the claim read on the PLIC. */
YanStatus yan_uart_read(YanUart *uart, uint32_t offset, uint32_t *value);
YanStatus yan_uart_write(YanUart *uart, uint32_t offset, uint32_t value);

/* Attach a backend, or detach with NULL. A backend missing either callback is
 * rejected with YAN_INVALID_ARGUMENT and the previous attachment is kept. */
YanStatus yan_uart_set_terminal(YanUart *uart, const YanUartTerminal *terminal);
/* True when a backend is attached. */
bool yan_uart_connected(const YanUart *uart);
/* True when writing TXDATA would be accepted right now; this is the same
 * predicate STATUS.TX_READY reports and the same one the write path applies. */
bool yan_uart_tx_ready(const YanUart *uart);

/* Host interface: hand one received byte to the guest. Refused with
 * YAN_UNAVAILABLE when no backend is attached. */
YanStatus yan_uart_push_rx(YanUart *uart, uint8_t byte);
/* Receive interrupt line as a pure function of device state: true while the
 * device is requesting service, false otherwise. Devices express a line level
 * and nothing else; the machine maps the line to a PLIC source and the PLIC
 * alone decides whether MEIP is produced. No side effects. */
bool yan_uart_pending(const YanUart *uart);

#endif
