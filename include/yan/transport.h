#ifndef YAN_TRANSPORT_H
#define YAN_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "yan/status.h"

/* General host transport channel: one ring buffer in each direction, control
 * registers in MMIO, and a doorbell. The device moves no bytes and interprets
 * none of them; guests write the rings with ordinary stores, the host reads
 * and writes the same RAM. See docs/specs/0014-host-transport-channel.md; this
 * header is the frozen interface and the implementation lives in
 * src/transport.c. */
#define YAN_TRANSPORT_BASE UINT32_C(0x10001000)
#define YAN_TRANSPORT_SIZE UINT32_C(0x00001000)

#define YAN_TRANSPORT_MAGIC UINT32_C(0x59414e31)
#define YAN_TRANSPORT_VERSION UINT32_C(0x00010000)

#define YAN_TRANSPORT_MAGIC_REG UINT32_C(0x000)
#define YAN_TRANSPORT_VERSION_REG UINT32_C(0x004)
#define YAN_TRANSPORT_STATUS UINT32_C(0x008)
#define YAN_TRANSPORT_RING_BASE UINT32_C(0x00c)
#define YAN_TRANSPORT_RING_SIZE UINT32_C(0x010)
#define YAN_TRANSPORT_G2H_HEAD UINT32_C(0x014)
#define YAN_TRANSPORT_G2H_TAIL UINT32_C(0x018)
#define YAN_TRANSPORT_H2G_HEAD UINT32_C(0x01c)
#define YAN_TRANSPORT_H2G_TAIL UINT32_C(0x020)
#define YAN_TRANSPORT_DOORBELL UINT32_C(0x024)
#define YAN_TRANSPORT_IRQ_STATUS UINT32_C(0x028)
#define YAN_TRANSPORT_IRQ_ENABLE UINT32_C(0x02c)

#define YAN_TRANSPORT_STATUS_HOST_READY UINT32_C(0x00000001)
#define YAN_TRANSPORT_STATUS_G2H_FULL UINT32_C(0x00000002)
#define YAN_TRANSPORT_STATUS_H2G_EMPTY UINT32_C(0x00000004)
#define YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED UINT32_C(0x00000008)

#define YAN_TRANSPORT_IRQ_H2G_DATA UINT32_C(0x00000001)

/* A ring needs a power-of-two size so the wrap is a mask, and one spare cell
 * so "empty" and "full" stay distinguishable. */
#define YAN_TRANSPORT_MIN_RING_SIZE UINT32_C(64)

typedef struct {
    uint32_t ring_base;
    uint32_t ring_size;
    uint32_t g2h_head;
    uint32_t g2h_tail;
    uint32_t h2g_head;
    uint32_t h2g_tail;
    uint32_t irq_status;
    uint32_t irq_enable;
    /* First detected guest overrun of the guest-to-host ring. Latched: the
     * channel only reports the fault, it does not repair the ring. */
    int overflow_detected;
    /* Host notification raised by a doorbell write. */
    void (*notify)(void *context);
    void *notify_context;
} YanTransport;

/* Reset clears the run state (heads, tails, pending interrupts and the overrun
 * latch) and keeps the host configuration, because the configuration belongs
 * to the host and not to the run. */
void yan_transport_reset(YanTransport *transport);
YanStatus yan_transport_read(YanTransport *transport, uint32_t offset,
                             uint32_t *value);
YanStatus yan_transport_write(YanTransport *transport, uint32_t offset,
                              uint32_t value);

/* Host interface. The ring lives in guest RAM, so the host moves the bytes
 * itself through the RAM accessors and only tells the device how far it got.
 * configure() validates the placement against the RAM window it is given. */
YanStatus yan_transport_configure(YanTransport *transport, uint32_t ring_base,
                                  uint32_t ring_size, uint32_t ram_base,
                                  uint32_t ram_size);
void yan_transport_set_notify(YanTransport *transport,
                              void (*notify)(void *context), void *context);
/* Bytes the host may read from the guest-to-host ring. */
uint32_t yan_transport_host_readable(const YanTransport *transport);
/* Bytes the host may write into the host-to-guest ring. */
uint32_t yan_transport_host_writable(const YanTransport *transport);
/* Advance the guest-to-host tail after the host consumed bytes. */
YanStatus yan_transport_host_consume(YanTransport *transport, uint32_t bytes);
/* Publish bytes the host wrote: advances the host-to-guest head and asserts
 * the host-to-guest interrupt. */
YanStatus yan_transport_host_publish(YanTransport *transport, uint32_t bytes);
/* Host-to-guest interrupt line as a pure function of device state: true while
 * the device is requesting service, false otherwise. Devices express a line
 * level and nothing else; the machine maps the line to a PLIC source and the
 * PLIC alone decides whether MEIP is produced. No side effects. */
bool yan_transport_pending(const YanTransport *transport);

#endif
