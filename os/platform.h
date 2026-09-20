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

/* Host transport channel: one byte ring in each direction, control registers in
 * MMIO and a doorbell. See docs/specs/0014-host-transport-channel.md for the
 * register map and docs/specs/0018-block-protocol.md for the first payload
 * carried on it.
 *
 * The names mirror include/yan/transport.h one for one under the YAN_OS_
 * prefix, so tests/test_boot.c can hold the two descriptions against each
 * other constant by constant. RING_BASE and RING_SIZE are register offsets and
 * not the ring itself: the values they hold are the ring's address in RAM and
 * the size of one ring. */
#define YAN_OS_TRANSPORT_BASE UINT32_C(0x10001000)
#define YAN_OS_TRANSPORT_SIZE UINT32_C(0x00001000)
#define YAN_OS_TRANSPORT_MAGIC UINT32_C(0x59414e31)
#define YAN_OS_TRANSPORT_VERSION UINT32_C(0x00010000)
#define YAN_OS_TRANSPORT_MAGIC_REG UINT32_C(0x000)
#define YAN_OS_TRANSPORT_VERSION_REG UINT32_C(0x004)
#define YAN_OS_TRANSPORT_STATUS UINT32_C(0x008)
#define YAN_OS_TRANSPORT_RING_BASE UINT32_C(0x00c)
#define YAN_OS_TRANSPORT_RING_SIZE UINT32_C(0x010)
#define YAN_OS_TRANSPORT_G2H_HEAD UINT32_C(0x014)
#define YAN_OS_TRANSPORT_G2H_TAIL UINT32_C(0x018)
#define YAN_OS_TRANSPORT_H2G_HEAD UINT32_C(0x01c)
#define YAN_OS_TRANSPORT_H2G_TAIL UINT32_C(0x020)
#define YAN_OS_TRANSPORT_DOORBELL UINT32_C(0x024)
#define YAN_OS_TRANSPORT_IRQ_STATUS UINT32_C(0x028)
#define YAN_OS_TRANSPORT_IRQ_ENABLE UINT32_C(0x02c)

#define YAN_OS_TRANSPORT_STATUS_HOST_READY UINT32_C(0x00000001)
#define YAN_OS_TRANSPORT_STATUS_G2H_FULL UINT32_C(0x00000002)
#define YAN_OS_TRANSPORT_STATUS_H2G_EMPTY UINT32_C(0x00000004)
#define YAN_OS_TRANSPORT_STATUS_OVERFLOW_DETECTED UINT32_C(0x00000008)
#define YAN_OS_TRANSPORT_IRQ_H2G_DATA UINT32_C(0x00000001)

/* Ring bytes are ordinary RAM, so they are reached with byte loads and stores;
 * the "4-byte accesses only" rule above governs the device window, not the
 * ring. RAM handles any width. */
static inline uint32_t yan_os_transport_status(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_STATUS);
}

/* The device is mapped for every run and reports HOST_READY = 0 until the host
 * configures a ring, so this is a usable probe: it never faults, and it is the
 * gate that must be open before anything below touches the rings. */
static inline int yan_os_transport_host_ready(void)
{
    return (yan_os_transport_status() & YAN_OS_TRANSPORT_STATUS_HOST_READY) != 0;
}

static inline uint32_t yan_os_transport_ring_base(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_RING_BASE);
}

static inline uint32_t yan_os_transport_ring_size(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_RING_SIZE);
}

/* G2H_TAIL and H2G_HEAD belong to the host and ignore guest writes, so this
 * header deliberately offers no setter for them: a store that is discarded
 * would look like it worked. Reads return the position the host maintains. */
static inline uint32_t yan_os_transport_g2h_head(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_G2H_HEAD);
}

static inline uint32_t yan_os_transport_g2h_tail(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_G2H_TAIL);
}

static inline uint32_t yan_os_transport_h2g_head(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_H2G_HEAD);
}

static inline uint32_t yan_os_transport_h2g_tail(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_H2G_TAIL);
}

static inline void yan_os_transport_set_g2h_head(uint32_t position)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_G2H_HEAD, position);
}

static inline void yan_os_transport_set_h2g_tail(uint32_t position)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_H2G_TAIL, position);
}

static inline uint32_t yan_os_transport_irq_status(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_IRQ_STATUS);
}

/* Write 1 to clear. The line is a level: it stays asserted while unread data
 * remains and the interrupt is enabled, so clearing the status is only half of
 * withdrawing it - the bytes have to be consumed as well (0014). */
static inline void yan_os_transport_irq_ack(void)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_IRQ_STATUS,
                        YAN_OS_TRANSPORT_IRQ_H2G_DATA);
}

/* Only bit 0 is defined; the remaining bits read 0 and are reserved. */
static inline void yan_os_transport_set_irq_enable(int enable)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_IRQ_ENABLE,
                        enable ? YAN_OS_TRANSPORT_IRQ_H2G_DATA : UINT32_C(0));
}

/* The ring stores must be visible before the position write that publishes
 * them (0014). YanCPU decodes FENCE and only advances the PC, so this holds the
 * contract without costing anything on today's single-hart model. */
static inline void yan_os_fence(void)
{
    __asm__ volatile ("fence" ::: "memory");
}

/* Byte offset of the ring's i-th slot. The ring size is a power of two, so the
 * wrap is a mask and the copy below may cross the wrap point without a second
 * code path at the call site. */
static inline uint32_t yan_os_ring_index(uint32_t base, uint32_t size, uint32_t i)
{
    return (base + i) & (size - UINT32_C(1));
}

/* Bytes the guest may still write into the guest-to-host ring. One cell is
 * always left free so that "empty" and "full" stay distinguishable, hence
 * size - 1 and not size. An unconfigured channel holds nothing and returns 0
 * rather than letting the mask arithmetic run on a zero size. */
static inline uint32_t yan_os_transport_g2h_space(void)
{
    const uint32_t size = yan_os_transport_ring_size();
    if (size == 0) {
        return 0;
    }
    const uint32_t used =
        (yan_os_transport_g2h_head() - yan_os_transport_g2h_tail()) & (size - UINT32_C(1));
    return size - UINT32_C(1) - used;
}

/* Bytes waiting in the host-to-guest ring. */
static inline uint32_t yan_os_transport_h2g_available(void)
{
    const uint32_t size = yan_os_transport_ring_size();
    if (size == 0) {
        return 0;
    }
    return (yan_os_transport_h2g_head() - yan_os_transport_h2g_tail()) & (size - UINT32_C(1));
}

/* Flow control belongs here and not at the call sites: 0014 requires that no
 * caller advance a ring position by hand.
 *
 * Both directions are all-or-nothing. When the ring cannot hold the whole of
 * `count` the copy does not start, nothing is written and no position moves,
 * which is what makes "the ring never contains half a frame" an invariant
 * instead of a hope. Returns 0 on success and -1 when the channel is
 * unconfigured or the bytes do not fit. */
static inline int yan_os_transport_g2h_push(const uint8_t *bytes, uint32_t count)
{
    const uint32_t size = yan_os_transport_ring_size();
    if (size == 0) {
        return -1;
    }
    const uint32_t mask = size - UINT32_C(1);
    const uint32_t head = yan_os_transport_g2h_head() & mask;
    const uint32_t used = (head - yan_os_transport_g2h_tail()) & mask;
    if (count > size - UINT32_C(1) - used) {
        return -1;
    }
    volatile uint8_t *ring = (volatile uint8_t *)(uintptr_t)yan_os_transport_ring_base();
    for (uint32_t i = 0; i < count; ++i) {
        ring[yan_os_ring_index(head, size, i)] = bytes[i];
    }
    yan_os_fence();
    yan_os_transport_set_g2h_head((head + count) & mask);
    return 0;
}

/* The host-to-guest ring starts where the guest-to-host ring ends; the two are
 * adjacent halves of one region (0014). */
static inline int yan_os_transport_h2g_pop(uint8_t *out, uint32_t count)
{
    const uint32_t size = yan_os_transport_ring_size();
    if (size == 0) {
        return -1;
    }
    const uint32_t mask = size - UINT32_C(1);
    const uint32_t tail = yan_os_transport_h2g_tail() & mask;
    const uint32_t available = (yan_os_transport_h2g_head() - tail) & mask;
    if (count > available) {
        return -1;
    }
    const volatile uint8_t *ring =
        (const volatile uint8_t *)(uintptr_t)(yan_os_transport_ring_base() + size);
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = ring[yan_os_ring_index(tail, size, i)];
    }
    yan_os_fence();
    yan_os_transport_set_h2g_tail((tail + count) & mask);
    return 0;
}

/* Ring the host's bell. The value is irrelevant; one write is one notification
 * and the host's callback does the work (0014). */
static inline void yan_os_transport_doorbell(void)
{
    yan_os_fence();
    YAN_OS_MMIO_WRITE32(YAN_OS_TRANSPORT_BASE + YAN_OS_TRANSPORT_DOORBELL, UINT32_C(1));
}

/* PLIC with one M-mode context, standard SiFive layout. See
 * docs/specs/0016-plic-gateway-and-irq-lines.md for the gateway semantics; the
 * source numbers this platform allocates are named above. Names mirror
 * include/yan/interrupt.h under the YAN_OS_ prefix. */
#define YAN_OS_PLIC_BASE UINT32_C(0x0c000000)
#define YAN_OS_PLIC_SIZE UINT32_C(0x00400000)
#define YAN_OS_PLIC_PRIORITY UINT32_C(0x0000)
#define YAN_OS_PLIC_PENDING UINT32_C(0x1000)
#define YAN_OS_PLIC_ENABLE_M UINT32_C(0x2000)
#define YAN_OS_PLIC_THRESHOLD_M UINT32_C(0x200000)
#define YAN_OS_PLIC_CLAIM_M UINT32_C(0x200004)
#define YAN_OS_PLIC_SOURCE_COUNT 32
#define YAN_OS_PLIC_SOURCE_MAX 31

/* A claim read is a handshake, not a peek: the source it returns leaves the
 * pending state and cannot become pending again until it is completed. A read
 * that returns 0 means nothing was pending for this context - the ordinary
 * outcome of a spurious external interrupt, and not an error. */
static inline uint32_t yan_os_plic_claim(void)
{
    return YAN_OS_MMIO_READ32(YAN_OS_PLIC_BASE + YAN_OS_PLIC_CLAIM_M);
}

/* Completing a source that is not in service does nothing, so completing 0 is
 * a no-op rather than a way to release an unrelated line. Completing a source
 * whose device is still requesting service re-pends it immediately: withdraw
 * the device condition first, then complete. */
static inline void yan_os_plic_complete(uint32_t source)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_PLIC_BASE + YAN_OS_PLIC_CLAIM_M, source);
}

/* Per-source priority. Source 0 is reserved and keeps its slot at zero. */
static inline void yan_os_plic_set_priority(uint32_t source, uint32_t priority)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_PLIC_BASE + YAN_OS_PLIC_PRIORITY + 4u * source, priority);
}

/* Enabling is a bit in a per-context bitmap, so this is a read-modify-write of
 * the containing word and leaves the other sources alone. Bit 0 is the
 * reserved source and is not settable. */
static inline void yan_os_plic_enable(uint32_t source, int enable)
{
    const uint32_t word = YAN_OS_PLIC_ENABLE_M + 4u * (source / 32u);
    const uint32_t bit = UINT32_C(1) << (source % 32u);
    uint32_t value = YAN_OS_MMIO_READ32(YAN_OS_PLIC_BASE + word);
    value = enable ? (value | bit) : (value & ~bit);
    YAN_OS_MMIO_WRITE32(YAN_OS_PLIC_BASE + word, value);
}

/* Sources at or below the threshold never reach this context. 0 lets
 * everything through that has a non-zero priority. */
static inline void yan_os_plic_set_threshold(uint32_t threshold)
{
    YAN_OS_MMIO_WRITE32(YAN_OS_PLIC_BASE + YAN_OS_PLIC_THRESHOLD_M, threshold);
}

#endif
