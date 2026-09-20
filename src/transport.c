#include "yan/transport.h"

#include <stddef.h>

/* Every register is a word and the Bus only routes word accesses here, but a
 * misaligned offset still has to be a fault of its own: the Bus is not the only
 * caller these accessors have. */
#define YAN_TRANSPORT_ALIGNMENT UINT32_C(4)

/* General host transport channel. The rings live in guest RAM and are moved by
 * ordinary loads and stores on both sides, so this device only keeps the
 * pointers, the interrupt line and the doorbell: it never moves a byte and
 * never looks at one. See docs/specs/0014-host-transport-channel.md.
 *
 * Both rings are circular and share RING_BASE, one after the other. A pointer
 * is a byte offset in [0, RING_SIZE), never an address, and the distance
 * between a head and a tail is taken modulo RING_SIZE. One cell is always left
 * free, so a ring holds RING_SIZE - 1 bytes and "empty" (HEAD == TAIL) stays
 * distinguishable from "full" ((HEAD + 1) % RING_SIZE == TAIL). */

static int transport_usable(const YanTransport *transport)
{
    /* HOST_READY is derived, never latched: the host must have placed a ring
     * and must be reachable through its notification callback. */
    return transport != NULL && transport->ring_size != 0 &&
           transport->notify != NULL;
}

static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size)
{
    return (head - tail) % size;
}

static uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t size)
{
    return size - 1 - ring_used(head, tail, size);
}

static uint32_t transport_status(const YanTransport *transport)
{
    uint32_t status = 0;
    if (transport_usable(transport)) {
        status |= YAN_TRANSPORT_STATUS_HOST_READY;
    }
    if (transport != NULL && transport->ring_size != 0) {
        if (ring_free(transport->g2h_head, transport->g2h_tail,
                      transport->ring_size) == 0) {
            status |= YAN_TRANSPORT_STATUS_G2H_FULL;
        }
        if (ring_used(transport->h2g_head, transport->h2g_tail,
                      transport->ring_size) == 0) {
            status |= YAN_TRANSPORT_STATUS_H2G_EMPTY;
        }
        if (transport->overflow_detected != 0) {
            status |= YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED;
        }
    }
    return status;
}

/* Reset clears the run state (heads, tails, pending interrupts and the overrun
 * latch) and keeps the host configuration, because the configuration belongs
 * to the host and not to the run. */
void yan_transport_reset(YanTransport *transport)
{
    if (transport == NULL) {
        return;
    }
    uint32_t ring_base = transport->ring_base;
    uint32_t ring_size = transport->ring_size;
    void (*notify)(void *) = transport->notify;
    void *context = transport->notify_context;
    *transport = (YanTransport){0};
    transport->ring_base = ring_base;
    transport->ring_size = ring_size;
    transport->notify = notify;
    transport->notify_context = context;
}

YanStatus yan_transport_read(YanTransport *transport, uint32_t offset,
                             uint32_t *value)
{
    if (transport == NULL || value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_TRANSPORT_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    switch (offset) {
    case YAN_TRANSPORT_MAGIC_REG:
        *value = YAN_TRANSPORT_MAGIC;
        return YAN_OK;
    case YAN_TRANSPORT_VERSION_REG:
        *value = YAN_TRANSPORT_VERSION;
        return YAN_OK;
    case YAN_TRANSPORT_STATUS:
        *value = transport_status(transport);
        return YAN_OK;
    case YAN_TRANSPORT_RING_BASE:
        *value = transport->ring_base;
        return YAN_OK;
    case YAN_TRANSPORT_RING_SIZE:
        *value = transport->ring_size;
        return YAN_OK;
    case YAN_TRANSPORT_G2H_HEAD:
        *value = transport->g2h_head;
        return YAN_OK;
    case YAN_TRANSPORT_G2H_TAIL:
        *value = transport->g2h_tail;
        return YAN_OK;
    case YAN_TRANSPORT_H2G_HEAD:
        *value = transport->h2g_head;
        return YAN_OK;
    case YAN_TRANSPORT_H2G_TAIL:
        *value = transport->h2g_tail;
        return YAN_OK;
    case YAN_TRANSPORT_DOORBELL:
        /* Write-only: a read reports nothing and does not ring. */
        *value = 0;
        return YAN_OK;
    case YAN_TRANSPORT_IRQ_STATUS:
        *value = transport->irq_status & YAN_TRANSPORT_IRQ_H2G_DATA;
        return YAN_OK;
    case YAN_TRANSPORT_IRQ_ENABLE:
        *value = transport->irq_enable & YAN_TRANSPORT_IRQ_H2G_DATA;
        return YAN_OK;
    default:
        break;
    }
    return YAN_UNMAPPED;
}

YanStatus yan_transport_write(YanTransport *transport, uint32_t offset,
                              uint32_t value)
{
    if (transport == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_TRANSPORT_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    switch (offset) {
    case YAN_TRANSPORT_G2H_HEAD:
        /* Guest production position: the only guest write into the pointers. */
        transport->g2h_head = value;
        return YAN_OK;
    case YAN_TRANSPORT_H2G_TAIL:
        /* Guest consumption position, driven by the guest like G2H_HEAD. */
        transport->h2g_tail = value;
        return YAN_OK;
    case YAN_TRANSPORT_DOORBELL:
        /* The guest rang. A doorbell carries no count, and a ring that looks
         * full here is either honest saturation or an overrun that wrapped
         * past it, so the two cannot be told apart from the pointers alone.
         * The overrun is therefore decided where the host states how many
         * bytes it moved, in the host interface below. */
        if (transport->notify != NULL) {
            transport->notify(transport->notify_context);
        }
        return YAN_OK;
    case YAN_TRANSPORT_IRQ_STATUS:
        /* Write one to clear bit 0; every other bit is reserved. */
        if ((value & YAN_TRANSPORT_IRQ_H2G_DATA) != 0) {
            transport->irq_status &= ~YAN_TRANSPORT_IRQ_H2G_DATA;
        }
        return YAN_OK;
    case YAN_TRANSPORT_IRQ_ENABLE:
        transport->irq_enable = value & YAN_TRANSPORT_IRQ_H2G_DATA;
        return YAN_OK;
    case YAN_TRANSPORT_MAGIC_REG:
    case YAN_TRANSPORT_VERSION_REG:
    case YAN_TRANSPORT_STATUS:
    case YAN_TRANSPORT_RING_BASE:
    case YAN_TRANSPORT_RING_SIZE:
    case YAN_TRANSPORT_G2H_TAIL:
    case YAN_TRANSPORT_H2G_HEAD:
        /* Constant or host driven, like the hardware-driven bits of mip: a
         * software write is accepted as a no-op and changes nothing. */
        return YAN_OK;
    default:
        break;
    }
    return YAN_UNMAPPED;
}

YanStatus yan_transport_configure(YanTransport *transport, uint32_t ring_base,
                                  uint32_t ring_size, uint32_t ram_base,
                                  uint32_t ram_size)
{
    if (transport == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    /* A power-of-two size keeps the wrap a mask; MIN_RING_SIZE keeps the spare
     * cell from dominating a small ring. */
    if (ring_size < YAN_TRANSPORT_MIN_RING_SIZE ||
        (ring_size & (ring_size - 1)) != 0) {
        return YAN_INVALID_ARGUMENT;
    }
    /* The base must be aligned to the size, otherwise the two rings do not
     * start on a ring boundary and the offsets stop being a mask. */
    if ((ring_base & (ring_size - 1)) != 0) {
        return YAN_INVALID_ARGUMENT;
    }
    /* Two rings must fit the window. Written as a division so a ring_size near
     * the top of the range cannot wrap when it is doubled. */
    if (ram_size < 2 || ring_size > ram_size / 2) {
        return YAN_INVALID_ARGUMENT;
    }
    const uint32_t base_end = ram_base + ram_size;
    if (ring_base < ram_base || ring_base >= base_end) {
        return YAN_INVALID_ARGUMENT;
    }
    /* Both rings, one after the other, must fit inside RAM. Written as a
     * distance so the sum cannot wrap. */
    if (ring_base - ram_base > ram_size - 2 * ring_size) {
        return YAN_INVALID_ARGUMENT;
    }

    transport->ring_base = ring_base;
    transport->ring_size = ring_size;
    /* Placement is a host action, so the run state it invalidates goes with it
     * rather than being carried across configurations. */
    transport->g2h_head = 0;
    transport->g2h_tail = 0;
    transport->h2g_head = 0;
    transport->h2g_tail = 0;
    transport->irq_status = 0;
    transport->irq_enable = 0;
    transport->overflow_detected = 0;
    return YAN_OK;
}

void yan_transport_set_notify(YanTransport *transport,
                              void (*notify)(void *context), void *context)
{
    if (transport == NULL) {
        return;
    }
    transport->notify = notify;
    transport->notify_context = context;
}

uint32_t yan_transport_host_readable(const YanTransport *transport)
{
    if (!transport_usable(transport)) {
        return 0;
    }
    return ring_used(transport->g2h_head, transport->g2h_tail, transport->ring_size);
}

uint32_t yan_transport_host_writable(const YanTransport *transport)
{
    if (!transport_usable(transport)) {
        return 0;
    }
    return ring_free(transport->h2g_head, transport->h2g_tail, transport->ring_size);
}

YanStatus yan_transport_host_consume(YanTransport *transport, uint32_t bytes)
{
    if (!transport_usable(transport)) {
        /* The call itself is legal; this channel has no ring to consume from
         * until the host places one and registers a callback. That is a
         * configuration gap, not a caller error and not a run-time fault. */
        return YAN_UNAVAILABLE;
    }
    if (bytes > yan_transport_host_readable(transport)) {
        /* The host would skip past bytes it never took, which is where a
         * guest-to-host overrun surfaces: the count it claims to have read
         * cannot fit in the room the ring had. The latch reports the fault and
         * the pointer stays where it was, because a silent repair would hide
         * the error. */
        transport->overflow_detected = 1;
        return YAN_INVALID_STATE;
    }
    transport->g2h_tail =
        (transport->g2h_tail + bytes) % transport->ring_size;
    return YAN_OK;
}

YanStatus yan_transport_host_publish(YanTransport *transport, uint32_t bytes)
{
    if (!transport_usable(transport)) {
        /* Same gap as consume(): nothing is broken, there is just no ring to
         * publish into yet. An over-large request on a live ring is a fault and
         * keeps YAN_INVALID_STATE below. */
        return YAN_UNAVAILABLE;
    }
    if (bytes > yan_transport_host_writable(transport)) {
        /* The host would move the head further than the free cells allow, so
         * the ring cannot hold what it is publishing. The latch reports it and
         * the pointer stays put, because a silent repair would hide the error. */
        transport->overflow_detected = 1;
        return YAN_INVALID_STATE;
    }
    if (bytes == 0) {
        /* Publishing nothing is not new data, so it must not raise the line. */
        return YAN_OK;
    }
    transport->h2g_head =
        (transport->h2g_head + bytes) % transport->ring_size;
    transport->irq_status |= YAN_TRANSPORT_IRQ_H2G_DATA;
    return YAN_OK;
}

bool yan_transport_pending(const YanTransport *transport)
{
    if (!transport_usable(transport)) {
        return false;
    }
    /* Pure level of the interrupt line: asserted while data is unread and the
     * interrupt is enabled, and dropped by the device state itself once either
     * stops holding. Mapping the line to a PLIC source belongs to the platform,
     * so no source number appears here. */
    return (transport->irq_status & YAN_TRANSPORT_IRQ_H2G_DATA) != 0 &&
           (transport->irq_enable & YAN_TRANSPORT_IRQ_H2G_DATA) != 0;
}
