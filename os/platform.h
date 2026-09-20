#ifndef YAN_OS_PLATFORM_H
#define YAN_OS_PLATFORM_H

#include <stdint.h>

/* YanOS's view of the platform MMIO window.
 *
 * YanOS is a freestanding Guest: it is compiled with -nostdlib and cannot
 * include the Host headers under include/yan, because those describe the
 * emulator's own translation units. The map is therefore restated here, exactly
 * as tests/guest/guest_devices.h restates CLINT and PLIC, and tests/test_boot.c
 * compares both descriptions constant by constant against the Host headers so
 * a restatement cannot drift silently.
 *
 * Every access below is a 32-bit word access. The devices answer word accesses
 * only and YanCPU traps any other width, so these accessors must not be
 * rewritten to byte or halfword operations. */

#define YAN_OS_MMIO_READ32(address) \
    (*(const volatile uint32_t *)(uintptr_t)(address))
#define YAN_OS_MMIO_WRITE32(address, value) \
    (*(volatile uint32_t *)(uintptr_t)(address) = (value))

/* UART character device. See docs/specs/0015-uart-device.md. */
#define YAN_OS_UART_BASE UINT32_C(0x10000000)
#define YAN_OS_UART_SIZE UINT32_C(0x00001000)
#define YAN_OS_UART_TXDATA UINT32_C(0x00)
#define YAN_OS_UART_RXDATA UINT32_C(0x04)
#define YAN_OS_UART_STATUS UINT32_C(0x08)
#define YAN_OS_UART_CONTROL UINT32_C(0x0c)
#define YAN_OS_UART_IRQ_STATUS UINT32_C(0x10)

#define YAN_OS_UART_STATUS_TX_READY UINT32_C(0x00000001)
#define YAN_OS_UART_STATUS_RX_READY UINT32_C(0x00000002)
#define YAN_OS_UART_STATUS_CONNECTED UINT32_C(0x00000004)
#define YAN_OS_UART_CONTROL_RX_IRQ_ENABLE UINT32_C(0x00000001)
#define YAN_OS_UART_IRQ_RX_PENDING UINT32_C(0x00000001)

/* Platform interrupt wiring. Source 0 is reserved; 1 is the host transport
 * channel and 2 is this UART. See docs/specs/0016. */
#define YAN_OS_PLIC_SOURCE_TRANSPORT UINT32_C(1)
#define YAN_OS_PLIC_SOURCE_UART UINT32_C(2)

static inline uint32_t yan_os_uart_status(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_UART_BASE + YAN_OS_UART_STATUS);
}

static inline int yan_os_uart_connected(void)
{
    return (yan_os_uart_status() & YAN_OS_UART_STATUS_CONNECTED) != 0;
}

/* True when writing TXDATA right now would be accepted. This is the same bit
 * the write path below consults, so "reported ready" and "accepted" cannot
 * disagree within one observation. */
static inline int yan_os_uart_tx_ready(void)
{
    return (yan_os_uart_status() & YAN_OS_UART_STATUS_TX_READY) != 0;
}

/* Returns 0 when the byte was accepted, -1 when the device refused it. A
 * refusal is never silent: the caller is expected to surface it rather than
 * spin. See the driver contract in docs/specs/0015-uart-device.md. */
static inline int yan_os_uart_put(uint8_t byte)
{
    if (!yan_os_uart_tx_ready()) {
        return -1;
    }
    YAN_OS_MMIO_WRITE32(YAN_OS_UART_BASE + YAN_OS_UART_TXDATA, byte);
    return 0;
}

/* Returns 0 and stores a byte when one was available, -1 when the receive
 * buffer is empty. Reading RXDATA clears RX_READY, so this never spins. */
static inline int yan_os_uart_get(uint8_t *byte)
{
    if ((yan_os_uart_status() & YAN_OS_UART_STATUS_RX_READY) == 0) {
        return -1;
    }
    *byte = (uint8_t)(YAN_OS_MMIO_READ32(YAN_OS_UART_BASE + YAN_OS_UART_RXDATA) &
                      UINT32_C(0xff));
    return 0;
}

#endif
