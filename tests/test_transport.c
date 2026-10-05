#include "yan/bus.h"
#include "yan/machine.h"
#include "yan/transport.h"
#include <stdio.h>

#include <stdio.h>

#include "unity.h"

/* Behaviour comes from docs/specs/0014-host-transport-channel.md. The register
 * and configuration cases use a bare Bus so the MMIO dispatch is exercised
 * directly; the ones that need the PLIC line or the whole-machine lifecycle use
 * a Machine, because that is where the wiring lives.
 *
 * The channel moves no bytes: the rings are ordinary guest RAM, so these cases
 * store and load them through the bus exactly as a guest would, and use the
 * host-side accessors only for the pointers and the counts. */

#define RAM_BASE UINT32_C(0x80000000)
/* Two rings of RING_SIZE plus a little room, so the placement validation has a
 * window that a valid configuration actually fits in. */
#define RAM_BYTES 512U
#define RING_BASE RAM_BASE
#define RING_SIZE UINT32_C(64)

static YanRam ram;
static YanBus bus;
static YanTransport channel;
static YanPlic plic;

static unsigned notify_calls;
static void *notify_context;

static YanMachine machine;
static YanMachine other;

static void counting_notify(void *context)
{
    ++notify_calls;
    notify_context = context;
}

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    channel = (YanTransport){0};
    plic = (YanPlic){0};
    notify_calls = 0;
    notify_context = NULL;

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, RAM_BYTES));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, RAM_BASE));
    yan_transport_reset(&channel);
    bus.transport = &channel;
}

void tearDown(void)
{
    /* Machine cases own a 32 MiB RAM each; releasing them here keeps a failing
     * assertion from turning into a leak. */
    yan_machine_destroy(&machine);
    yan_machine_destroy(&other);
    machine = (YanMachine){0};
    other = (YanMachine){0};
    yan_ram_destroy(&ram);
}

static uint32_t read_word(uint32_t address)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&bus, address, 4, &value).status);
    return value;
}

static void write_word(uint32_t address, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&bus, address, 4, value).status);
}

/* The Machine owns its own RAM and devices, so its registers are only reachable
 * through its own Bus. */
static uint32_t read_machine_word(YanMachine *target, uint32_t address)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_read(&target->bus, address, 4, &value).status);
    return value;
}

static void write_machine_word(YanMachine *target, uint32_t address, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&target->bus, address, 4, value).status);
}

/* Guest stores into the ring: plain RAM through the data path. */
static void ring_store(uint32_t offset, uint8_t byte)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, RING_BASE + offset, 1, byte).status);
}

static uint8_t ring_load(uint32_t offset)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_read(&bus, RING_BASE + offset, 1, &value).status);
    return (uint8_t)value;
}

/* A guest that produced `bytes` stores the ring bytes and then advances its
 * head, which is the order the specification requires of the os/ channel. */
static void guest_produce_g2h(uint32_t bytes)
{
    const uint32_t head = channel.g2h_head;
    for (uint32_t index = 0; index < bytes; ++index) {
        ring_store((head + index) % RING_SIZE, (uint8_t)index);
    }
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, head + bytes);
}

/* The host consumes by advancing the host-side tail; the bytes themselves never
 * pass through the device. */
static void host_drain_g2h(uint32_t bytes)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_consume(&channel, bytes));
}

static void configure_default(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_configure(&channel, RING_BASE,
                                                          RING_SIZE, RAM_BASE,
                                                          RAM_BYTES));
}

static void identity_registers_are_constant(void)
{
    TEST_ASSERT_EQUAL_HEX32(
        UINT32_C(0x59414e31),
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_MAGIC_REG));
    TEST_ASSERT_EQUAL_HEX32(
        UINT32_C(0x00010000),
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_VERSION_REG));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_MAGIC,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_MAGIC_REG));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_VERSION,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_VERSION_REG));

    /* DOORBELL is write-only: a read has nothing to report and must not ring. */
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL));
    TEST_ASSERT_EQUAL_UINT(0, notify_calls);
}

static void host_ready_is_derived_from_the_configuration(void)
{
    /* A freshly reset channel is inert: nothing is usable yet. */
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(0, channel.ring_size);
    TEST_ASSERT_NULL(channel.notify);
    TEST_ASSERT_NULL(channel.notify_context);
    TEST_ASSERT_FALSE(yan_transport_pending(&channel));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&channel));

    /* Placement alone does not make the channel ready: the host must also be
     * reachable, which is what the notify callback means. */
    configure_default();
    TEST_ASSERT_EQUAL_HEX32(
        0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
               YAN_TRANSPORT_STATUS_HOST_READY);

    /* Until the host is reachable the ring semantics are undefined, so the
     * status word reports nothing at all rather than inventing "empty". */
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, channel.overflow_detected);

    /* A latched overrun is held back with the other ring bits while no host is
     * reachable, and comes back with them once one is. */
    channel.overflow_detected = 1;
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));

    yan_transport_set_notify(&channel, counting_notify, &channel);
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY |
            YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(RING_BASE,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_BASE));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_SIZE));

    /* HOST_READY is a function of the configuration and never a latch: it is
     * reported for exactly as long as both halves hold, and clearing either
     * half clears it. The test below zeroes the size rather than setting an
     * invalid one, because picking a valid size is configure()'s job. */
    yan_transport_set_notify(&channel, NULL, NULL);
    TEST_ASSERT_EQUAL_HEX32(
        0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
               YAN_TRANSPORT_STATUS_HOST_READY);
    channel.ring_size = 0;
    yan_transport_set_notify(&channel, counting_notify, &channel);
    TEST_ASSERT_EQUAL_HEX32(
        0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
               YAN_TRANSPORT_STATUS_HOST_READY);
    yan_transport_set_notify(NULL, counting_notify, NULL);
}

static void read_only_registers_ignore_writes(void)
{
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);

    const uint32_t offsets[] = {
        YAN_TRANSPORT_MAGIC_REG, YAN_TRANSPORT_VERSION_REG, YAN_TRANSPORT_STATUS,
        YAN_TRANSPORT_RING_BASE, YAN_TRANSPORT_RING_SIZE, YAN_TRANSPORT_G2H_TAIL,
        YAN_TRANSPORT_H2G_HEAD,
    };
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        const uint32_t before = read_word(YAN_TRANSPORT_BASE + offsets[index]);
        write_word(YAN_TRANSPORT_BASE + offsets[index], UINT32_C(0xa5a5a5a5));
        TEST_ASSERT_EQUAL_HEX32(before, read_word(YAN_TRANSPORT_BASE + offsets[index]));
    }
    TEST_ASSERT_EQUAL_HEX32(RING_BASE, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE, channel.ring_size);
    TEST_ASSERT_EQUAL_HEX32(0, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_head);

    /* The write-only doorbell is not an exception: writing it rings the host
     * once, and never leaves a value behind for a later read. */
    TEST_ASSERT_EQUAL_UINT(0, notify_calls);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, UINT32_C(0xa5a5a5a5));
    TEST_ASSERT_EQUAL_UINT(1, notify_calls);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL));
    TEST_ASSERT_EQUAL_UINT(1, notify_calls);

    /* The two guest-driven pointers are the writable pair, and they come back
     * as the guest left them. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, 7);
    TEST_ASSERT_EQUAL_HEX32(7, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(7, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD));
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL, 5);
    TEST_ASSERT_EQUAL_HEX32(5, channel.h2g_tail);
    TEST_ASSERT_EQUAL_HEX32(5, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL));
}

static void configuration_validates_placement(void)
{
    /* A ring needs a power-of-two size of at least 64 bytes and room for both
     * rings inside the RAM window. Nothing else: the ring offset is taken
     * modulo the size, so where the base sits inside the window is free, and a
     * base that is not aligned to the size is legal. None of the rejected
     * shapes may disturb the standing configuration. */
    const uint32_t bad_sizes[] = {0,          1,  63, 65, 96, 128 + 64, UINT32_MAX,
                                  UINT32_C(0x80000000)};
    for (size_t index = 0; index < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++index) {
        TEST_ASSERT_EQUAL_INT(
            YAN_INVALID_ARGUMENT,
            yan_transport_configure(&channel, RING_BASE, bad_sizes[index], RAM_BASE,
                                    RAM_BYTES));
    }
    /* An unaligned base that really does run off the end is still rejected. */
    TEST_ASSERT_EQUAL_INT(
        YAN_INVALID_ARGUMENT,
        yan_transport_configure(&channel, RING_BASE + 32, 256, RAM_BASE, RAM_BYTES));
    /* The two rings run off the end of RAM. */
    TEST_ASSERT_EQUAL_INT(
        YAN_INVALID_ARGUMENT,
        yan_transport_configure(&channel, RING_BASE + 128, 256, RAM_BASE, RAM_BYTES));
    /* The base itself is outside the RAM window. */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_transport_configure(&channel, RING_BASE - 4, 64, RAM_BASE,
                                                  RAM_BYTES));
    TEST_ASSERT_EQUAL_INT(
        YAN_INVALID_ARGUMENT,
        yan_transport_configure(&channel, RAM_BASE + RAM_BYTES, 64, RAM_BASE, RAM_BYTES));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_transport_configure(&channel, RING_BASE, 64, RAM_BASE,
                                                  UINT32_C(0)));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_transport_configure(&channel, RING_BASE, 64, RAM_BASE,
                                                  UINT32_C(1)));

    /* A rejected configuration is not a half-applied one. */
    TEST_ASSERT_EQUAL_HEX32(0, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(0, channel.ring_size);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_BASE));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_SIZE));
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_tail);

    /* An unaligned base inside the window is a legal configuration, and the
     * ring it starts behaves like any other. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_configure(&channel, RING_BASE + 32,
                                                          RING_SIZE, RAM_BASE,
                                                          RAM_BYTES));
    TEST_ASSERT_EQUAL_HEX32(RING_BASE + 32, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(RING_BASE + 32,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_BASE));
    yan_transport_set_notify(&channel, counting_notify, &channel);
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    guest_produce_g2h(4);
    TEST_ASSERT_EQUAL_HEX32(4, yan_transport_host_readable(&channel));
    host_drain_g2h(4);
    /* Fill from wherever the tail ended up: a full ring is head - tail ==
     * RING_SIZE - 1, which the absolute register value does not decide. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD,
               channel.g2h_tail + RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY |
            YAN_TRANSPORT_STATUS_G2H_FULL,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    host_drain_g2h(RING_SIZE - 1);
    yan_transport_set_notify(&channel, NULL, NULL);
    yan_transport_reset(&channel);

    /* The largest ring this window fits. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_configure(&channel, RAM_BASE,
                                                          RAM_BYTES / 2, RAM_BASE,
                                                          RAM_BYTES));
    TEST_ASSERT_EQUAL_HEX32(RAM_BYTES / 2, channel.ring_size);
    TEST_ASSERT_EQUAL_HEX32(RAM_BYTES / 2,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_SIZE));
    /* One power of two larger and the second ring runs off the end. */
    TEST_ASSERT_EQUAL_INT(
        YAN_INVALID_ARGUMENT,
        yan_transport_configure(&channel, RAM_BASE, RAM_BYTES, RAM_BASE, RAM_BYTES));

    /* Nothing but a good configuration starts a ring. */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_transport_configure(NULL, RING_BASE, RING_SIZE, RAM_BASE,
                                                  RAM_BYTES));

    /* Before a configuration exists the host side has no ring to move bytes
     * through. The calls are legal, so the answer is "not available" rather
     * than a caller error or a run-time fault. */
    YanTransport bare = {0};
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_consume(&bare, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_publish(&bare, 1));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&bare));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&bare));
    TEST_ASSERT_FALSE(yan_transport_pending(&bare));

    /* Placement without a callback is the other half of the same gap: the ring
     * exists but no host is reachable to hand bytes to. */
    bare.ring_size = RING_SIZE;
    bare.ring_base = RING_BASE;
    TEST_ASSERT_NULL(bare.notify);
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_consume(&bare, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNAVAILABLE, yan_transport_host_publish(&bare, 1));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&bare));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&bare));
    TEST_ASSERT_FALSE(yan_transport_pending(&bare));

    /* With both halves in place the same calls stop being unavailable, and a
     * request the ring cannot satisfy is a run-time fault instead. */
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_consume(&channel, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE,
                          yan_transport_host_consume(&channel, 1));
    /* The refusal on a live ring is the overrun report, so the latch is set:
     * that is exactly what separates it from the YAN_UNAVAILABLE above. */
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);
    yan_transport_set_notify(NULL, counting_notify, NULL);
    yan_transport_reset(NULL);
}

static void rings_start_empty_and_round_trip_bytes(void)
{
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);

    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_STATUS_HOST_READY |
                                YAN_TRANSPORT_STATUS_H2G_EMPTY,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));

    /* The host publishes five bytes; the host-to-guest side stops being empty
     * and the guest sees the new head, with no byte passing through the device. */
    for (uint32_t index = 0; index < 5; ++index) {
        ring_store(RING_SIZE + index, (uint8_t)(0xf0 + index));
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 5));
    TEST_ASSERT_EQUAL_HEX32(5, channel.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(5, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_HEAD));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_STATUS_HOST_READY,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1 - 5, yan_transport_host_writable(&channel));

    /* The guest consumes in order: the ring holds the host's bytes verbatim. */
    for (uint32_t index = 0; index < 5; ++index) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xf0 + index),
                                ring_load(RING_SIZE + index));
    }
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL, 5);
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_STATUS_HOST_READY |
                                YAN_TRANSPORT_STATUS_H2G_EMPTY,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));

    /* Guest to host runs the same way through the two guest-side registers. */
    guest_produce_g2h(4);
    TEST_ASSERT_EQUAL_HEX32(4, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_TAIL));
    host_drain_g2h(4);
    TEST_ASSERT_EQUAL_HEX32(4, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_TAIL));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
}

static void status_reports_full_and_empty_edges(void)
{
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);

    /* An empty ring is never full. */
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
                                   YAN_TRANSPORT_STATUS_G2H_FULL);

    /* The capacity of a ring is RING_SIZE - 1: one cell stays free so empty and
     * full remain distinguishable. Filling the guest-to-host ring to the last
     * usable cell leaves it one step short of head == tail. */
    guest_produce_g2h(RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY |
            YAN_TRANSPORT_STATUS_G2H_FULL,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_UINT(0, notify_calls);

    /* Draining it back to empty clears the full bit and reopens the capacity. */
    host_drain_g2h(RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));

    /* One cell short of full is not full: at RING_SIZE - 2 bytes the guest can
     * still advance once more. */
    guest_produce_g2h(RING_SIZE - 2);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
                                   YAN_TRANSPORT_STATUS_G2H_FULL);
    guest_produce_g2h(1);
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY |
            YAN_TRANSPORT_STATUS_G2H_FULL,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));

    /* The host-to-guest ring reaches the same edge: at RING_SIZE - 1 published
     * bytes there is no room left, and one consumption reopens a single byte.
     * The guest-to-host ring is freed first, because a host-side publish is
     * refused while it is full: that is exactly the overrun check below. */
    host_drain_g2h(RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_transport_host_publish(&channel, RING_SIZE - 1));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_consume(&channel, 0));
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL, 1);
    TEST_ASSERT_EQUAL_HEX32(1, yan_transport_host_writable(&channel));

    /* A request past the free space is refused and moves nothing. */
    const uint32_t head = channel.h2g_head;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_publish(&channel, 2));
    TEST_ASSERT_EQUAL_HEX32(head, channel.h2g_head);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_consume(&channel, 1));
}

static void ring_wrap_and_byte_order(void)
{
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);

    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));

    /* Fill the ring to capacity, so the wrap cases below start from a ring
     * that was exercised at its edge. */
    guest_produce_g2h(RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel) - (RING_SIZE - 1));
    host_drain_g2h(RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));

    /* Twenty-four bytes land at offsets 60, 61, 62, 63, 0..19: the ring is
     * empty again after the drain above, so the head starts from zero. */
    const uint32_t produced = 24;
    guest_produce_g2h(produced);
    /* The pointers are byte offsets, not normalised counters: after the drain
     * the tail is at 63, so the head walks 63, 64.. and the location is the
     * offset modulo the ring size. */
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1 + produced, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(produced, yan_transport_host_readable(&channel));
    for (uint32_t index = 0; index < produced; ++index) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)index,
                                ring_load((RING_SIZE - 1 + index) % RING_SIZE));
    }
    host_drain_g2h(produced);

    /* Park the ring at the seam: the guest sets its own head, and the host
     * drains everything up to that offset through its accessor, because
     * G2H_TAIL is host driven and a store to it is ignored. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, RING_SIZE - 2);
    const uint32_t tail_before = read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_TAIL);
    /* A store to the host-driven tail is ignored, so it still reads the value
     * the host left behind. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_TAIL, RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(tail_before,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_TAIL));
    TEST_ASSERT_EQUAL_HEX32(tail_before, channel.g2h_tail);
    host_drain_g2h(yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 2, channel.g2h_tail);

    /* Four bytes at the seam: two land before the end and two at the front. */
    guest_produce_g2h(4);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 2 + 4, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(2, channel.g2h_head % RING_SIZE);
    TEST_ASSERT_EQUAL_HEX32(4, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_UINT8(0, ring_load(RING_SIZE - 2));
    TEST_ASSERT_EQUAL_UINT8(1, ring_load(RING_SIZE - 1));
    TEST_ASSERT_EQUAL_UINT8(2, ring_load(0));
    TEST_ASSERT_EQUAL_UINT8(3, ring_load(1));
    host_drain_g2h(4);

    /* The same wrap rule drives the host-to-guest ring. The guest parks its own
     * tail at the seam; H2G_HEAD is host driven, so it is read and not written,
     * and a store to it is ignored. */
    const uint32_t h2g_head_before =
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_HEAD);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL, RING_SIZE - 2);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_HEAD, 0);
    TEST_ASSERT_EQUAL_HEX32(h2g_head_before,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_HEAD));
    const uint32_t free_before = yan_transport_host_writable(&channel);
    for (uint32_t index = 0; index < 8; ++index) {
        ring_store((RING_SIZE - 2 + index) % RING_SIZE, (uint8_t)(0x40 + index));
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 8));
    TEST_ASSERT_EQUAL_HEX32(h2g_head_before + 8, channel.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(free_before - 8, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_HEX32(8 % RING_SIZE, channel.h2g_head % RING_SIZE);
    for (uint32_t index = 0; index < 8; ++index) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x40 + index),
                                ring_load((RING_SIZE - 2 + index) % RING_SIZE));
    }
}

static void overrun_latches_the_first_error(void)
{
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);

    /* The guest fills the ring to capacity, then announces four more bytes and
     * rings the doorbell. The device cannot stop a store into RAM, so the
     * overrun shows up as a count the host cannot honour: the ring has no room
     * for those bytes. Standing in for the os/ wrapper, the host asks to take
     * the four bytes it was told about. */
    guest_produce_g2h(RING_SIZE - 1);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY |
            YAN_TRANSPORT_STATUS_G2H_FULL,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));

    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, RING_SIZE - 1 + 4);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_UINT(1, notify_calls);
    TEST_ASSERT_EQUAL_HEX32(
        0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
               YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED);

    /* The announced four bytes wrap past the ring, so only three sit between
     * head and tail: one byte short of what the guest claimed. The refusal is
     * what sets the latch; the pointers stay where they were. */
    TEST_ASSERT_EQUAL_HEX32(3, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_consume(&channel, 4));
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(0, channel.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1 + 4, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY |
            YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));

    /* The head keeps whatever the guest wrote: the channel reports and does not
     * repair. Ringing again neither clears the latch nor moves a pointer. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 7);
    TEST_ASSERT_EQUAL_UINT(2, notify_calls);
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1 + 4,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD));

    /* Draining reports it once more; the host still consumes exactly what it
     * asked for, and the head keeps whatever the guest wrote. */
    const uint32_t head = channel.g2h_head;
    host_drain_g2h(3);
    TEST_ASSERT_EQUAL_HEX32(head, channel.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(3, channel.g2h_tail);

    /* A fresh channel starts with the latch clear, so the first error is the
     * one reported and not something carried over from another run. */
    yan_transport_reset(&channel);
    TEST_ASSERT_EQUAL_INT(0, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(
        0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS) &
               YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED);

    /* An honest ring is not a fault: a publish that fills the last usable cell
     * is accepted, and the next one has nowhere to go. */
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_HEX32(RING_SIZE - 1, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_transport_host_publish(&channel, RING_SIZE - 1));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_writable(&channel));
    TEST_ASSERT_EQUAL_INT(0, channel.overflow_detected);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_publish(&channel, 1));
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);

    /* The mirror fault: a host that advances the tail past the readable bytes.
     * The latch already holds the first error, so it keeps it and the tail. */
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&channel));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_transport_host_consume(&channel, 1));
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);
    const uint32_t tail_after_drain = channel.g2h_tail;
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_OVERFLOW_DETECTED,
        read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(tail_after_drain, channel.g2h_tail);

    /* The latch is a report and not a counter: it stays at one, and a reset is
     * the only thing that clears it. */
    TEST_ASSERT_EQUAL_INT(1, channel.overflow_detected);
    const uint32_t saved_base = channel.ring_base;
    const uint32_t saved_size = channel.ring_size;
    yan_transport_reset(&channel);
    TEST_ASSERT_EQUAL_INT(0, channel.overflow_detected);
    TEST_ASSERT_EQUAL_HEX32(saved_base, channel.ring_base);
    TEST_ASSERT_EQUAL_HEX32(saved_size, channel.ring_size);
    TEST_ASSERT_FALSE(yan_transport_pending(&channel));
}

static void doorbell_notifies_the_registered_host(void)
{
    configure_default();

    /* An unconfigured host is not notified: there is nowhere to deliver. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_UINT(0, notify_calls);

    yan_transport_set_notify(&channel, counting_notify, &channel);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 0);
    TEST_ASSERT_EQUAL_UINT(1, notify_calls);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT(2, notify_calls);
    TEST_ASSERT_EQUAL_PTR(&channel, notify_context);

    /* Registering another callback replaces both parts of the pair. */
    yan_transport_set_notify(&channel, counting_notify, &machine);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 0);
    TEST_ASSERT_EQUAL_UINT(3, notify_calls);
    TEST_ASSERT_EQUAL_PTR(&machine, notify_context);

    /* The callback is not a ring: a doorbell nobody answers consumes nothing. */
    yan_transport_set_notify(&channel, NULL, NULL);
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_UINT(3, notify_calls);
    TEST_ASSERT_NULL(channel.notify_context);
}

static void irq_status_is_write_one_to_clear(void)
{
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);

    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(1, channel.irq_enable);
    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE));

    /* A doorbell does not assert the host-to-guest line by itself. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS));
    TEST_ASSERT_FALSE(yan_transport_pending(&channel));

    /* Writing zero to a write-one-to-clear register clears nothing. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS, 0);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 3));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_IRQ_H2G_DATA,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS));
    TEST_ASSERT_TRUE(yan_transport_pending(&channel));

    /* Reserved bits ignore writes; writing one to bit 0 clears only that bit. */
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS, UINT32_C(0xfffffffe));
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_IRQ_H2G_DATA, channel.irq_status);
    TEST_ASSERT_TRUE(yan_transport_pending(&channel));
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS, 1);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS));
    TEST_ASSERT_FALSE(yan_transport_pending(&channel));

    /* A line with data but no enable stays silent, and re-enabling shows it. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&channel, 2));
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_IRQ_H2G_DATA, channel.irq_status);
    TEST_ASSERT_FALSE(yan_transport_pending(&channel));
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 1);
    TEST_ASSERT_TRUE(yan_transport_pending(&channel));
    write_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS, 1);

    /* An unconfigured channel has no usable line even with the bits set. */
    YanTransport bare = channel;
    bare.ring_size = 0;
    bare.irq_status = YAN_TRANSPORT_IRQ_H2G_DATA;
    bare.irq_enable = 1;
    TEST_ASSERT_FALSE(yan_transport_pending(&bare));
}

static void window_errors_and_host_arguments(void)
{
    uint32_t value = UINT32_C(0x12345678);
    YanBus plain = {0};

    /* The transport window answers 4-byte words only; other widths stay on the
     * path they came from, and unimplemented offsets are not mapped at all. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_TRANSPORT_BASE, 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_TRANSPORT_BASE, 2, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, YAN_TRANSPORT_BASE, 2, 0).status);
    TEST_ASSERT_EQUAL_INT(
        YAN_UNMAPPED,
        yan_bus_read(&bus, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE + 4, 4, &value)
            .status);
    TEST_ASSERT_EQUAL_INT(
        YAN_UNMAPPED,
        yan_bus_write(&bus, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE + 4, 4, 0)
            .status);
    TEST_ASSERT_EQUAL_INT(
        YAN_UNMAPPED,
        yan_bus_read(&bus, YAN_TRANSPORT_BASE + YAN_TRANSPORT_SIZE, 4, &value).status);
    /* Every word from the first unimplemented offset to the end of the window
     * is unmapped; only the write-only doorbell reads as zero from the
     * implemented set. */
    for (uint32_t offset = YAN_TRANSPORT_IRQ_ENABLE + 4; offset < YAN_TRANSPORT_SIZE;
         offset += 4) {
        TEST_ASSERT_EQUAL_INT(
            YAN_UNMAPPED,
            yan_bus_read(&bus, YAN_TRANSPORT_BASE + offset, 4, &value).status);
    }
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_TRANSPORT_BASE - 4, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_fetch32(&bus, YAN_TRANSPORT_BASE, &value).status);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x12345678), value);

    /* An unaligned word is a fault of its own, and the word never reaches the
     * device: no register changes and no doorbell rings. */
    configure_default();
    yan_transport_set_notify(&channel, counting_notify, &channel);
    const uint32_t status = read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_read(&bus, YAN_TRANSPORT_BASE + 2, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(
        YAN_UNALIGNED,
        yan_bus_write(&bus, YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL + 2, 4, 1)
            .status);
    TEST_ASSERT_EQUAL_UINT(0, notify_calls);
    TEST_ASSERT_EQUAL_HEX32(status,
                            read_word(YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, channel.irq_status);
    TEST_ASSERT_EQUAL_HEX32(0, channel.h2g_head);

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_transport_read(NULL, 0, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_transport_read(&channel, 0, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_transport_write(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_transport_read(&channel, YAN_TRANSPORT_SIZE, &value));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_transport_write(&channel, YAN_TRANSPORT_SIZE, 0));

    /* With no transport attached the window is not mapped on that Bus, so the
     * address falls through to the RAM-only path. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&plain, &ram, RAM_BASE));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&plain, YAN_TRANSPORT_BASE, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&plain, YAN_TRANSPORT_BASE, 4, 1).status);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&plain));
}

static void irq_line_drives_plic_source_one(void)
{
    machine = (YanMachine){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));

    TEST_ASSERT_EQUAL_INT(
        YAN_OK, yan_transport_configure(&machine.transport, YAN_RAM_BASE,
                                        UINT32_C(256), YAN_RAM_BASE, YAN_RAM_SIZE));
    yan_transport_set_notify(&machine.transport, counting_notify, &machine);

    /* The device reports a line level and nothing else: with no unread data
     * there is nothing to request, whatever the enable says. */
    TEST_ASSERT_FALSE(yan_transport_pending(&machine.transport));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&machine.plic));

    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 1);

    /* Data arrives, so the line goes high and the machine maps it to the
     * transport's PLIC source. Priority zero still means "never interrupts", so
     * the source is pending without producing MEIP yet. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&machine.transport, 3));
    TEST_ASSERT_TRUE(yan_transport_pending(&machine.transport));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(1U << YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&machine.plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mip);

    /* Give the source a priority and enable it for this context: the pending
     * source now selects, so the platform line reaches mip.MEIP. */
    write_machine_word(
        &machine, YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4 * YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
        1);
    write_machine_word(&machine, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M,
                       1U << YAN_MACHINE_PLIC_SOURCE_TRANSPORT);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&machine.plic));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_bus_pending_interrupts(&machine.bus));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, machine.cpu.csr.mip);

    /* An unclaimed request follows its line: the handler drains the ring and
     * clears the status word, the device line drops, and the next sample drops
     * the source with it. */
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL, 3);
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS, 1);
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.irq_status);
    TEST_ASSERT_FALSE(yan_transport_pending(&machine.transport));
    TEST_ASSERT_EQUAL_HEX32(
        0, read_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_STATUS));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&machine.plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mip);

    /* New data raises the line again, and the machine re-asserts the source. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&machine.transport, 4));
    TEST_ASSERT_TRUE(yan_transport_pending(&machine.transport));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(1U << YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&machine.plic));

    /* A claimed source is held closed while it is in service: driving the line
     * again cannot re-enter the handler before the handler completes. */
    TEST_ASSERT_EQUAL_HEX32(YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            read_machine_word(&machine, YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(1U << YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            machine.plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&machine.plic));

    /* Completing it re-samples the line: the device still wants service, so the
     * source pends again and MEIP follows. */
    write_machine_word(&machine, YAN_PLIC_BASE + YAN_PLIC_CLAIM_M,
                       YAN_MACHINE_PLIC_SOURCE_TRANSPORT);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(1U << YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&machine.plic));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, machine.cpu.csr.mip);

    /* Disabling the interrupt in the device drops the line even though the
     * status word still says there is unread data, so the source withdraws. */
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_IRQ_H2G_DATA, machine.transport.irq_status);
    TEST_ASSERT_FALSE(yan_transport_pending(&machine.transport));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&machine.plic));
}

static void machine_reset_keeps_the_host_configuration(void)
{
    machine = (YanMachine){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));

    /* The Machine wired the device and reset it, so the channel starts inert. */
    TEST_ASSERT_EQUAL_HEX32(0, read_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_PTR(&machine.transport, machine.bus.transport);

    TEST_ASSERT_EQUAL_INT(
        YAN_OK, yan_transport_configure(&machine.transport, YAN_RAM_BASE,
                                        UINT32_C(256), YAN_RAM_BASE, YAN_RAM_SIZE));
    yan_transport_set_notify(&machine.transport, counting_notify, &machine);

    /* Run state accumulated during a run. */
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, 11);
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_H2G_TAIL, 7);
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 1);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&machine.transport, 5));
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, 300);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE,
                          yan_transport_host_publish(&machine.transport, 64));
    TEST_ASSERT_EQUAL_INT(1, machine.transport.overflow_detected);
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_UINT(1, notify_calls);
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_IRQ_H2G_DATA, machine.transport.irq_status);

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.g2h_tail);
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.h2g_tail);
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.irq_status);
    TEST_ASSERT_EQUAL_HEX32(0, machine.transport.irq_enable);
    TEST_ASSERT_EQUAL_INT(0, machine.transport.overflow_detected);

    /* The host's configuration is not run state: placement, size and callback
     * survive, so HOST_READY stays asserted and both rings read empty. */
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE,
                            read_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_BASE));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(256),
                            read_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_RING_SIZE));
    TEST_ASSERT_EQUAL_PTR(counting_notify, machine.transport.notify);
    TEST_ASSERT_EQUAL_PTR(&machine, machine.transport.notify_context);
    TEST_ASSERT_EQUAL_HEX32(
        YAN_TRANSPORT_STATUS_HOST_READY | YAN_TRANSPORT_STATUS_H2G_EMPTY,
        read_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_STATUS));
    TEST_ASSERT_EQUAL_HEX32(0, yan_transport_host_readable(&machine.transport));
    TEST_ASSERT_EQUAL_HEX32(255, yan_transport_host_writable(&machine.transport));
    TEST_ASSERT_FALSE(yan_transport_pending(&machine.transport));

    /* The doorbell still reaches the host after a reset. */
    const unsigned before_ring = notify_calls;
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_DOORBELL, 1);
    TEST_ASSERT_EQUAL_UINT(before_ring + 1, notify_calls);

    /* Destroying a machine detaches the device and releases its RAM; tearDown
     * repeats it, which is a no-op on an already destroyed machine. */
    yan_machine_destroy(&machine);
    TEST_ASSERT_NULL(machine.transport.notify);
    TEST_ASSERT_NULL(machine.bus.transport);
    TEST_ASSERT_NULL(machine.bus.ram);
    TEST_ASSERT_EQUAL_INT(0, machine.ram.size);
    TEST_ASSERT_NULL(machine.ram.data);
}

static void machine_isolation_and_destroy(void)
{
    machine = (YanMachine){0};
    other = (YanMachine){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&other));

    TEST_ASSERT_EQUAL_INT(
        YAN_OK, yan_transport_configure(&machine.transport, YAN_RAM_BASE,
                                        UINT32_C(128), YAN_RAM_BASE, YAN_RAM_SIZE));
    yan_transport_set_notify(&machine.transport, counting_notify, &machine);
    TEST_ASSERT_EQUAL_INT(
        YAN_OK, yan_transport_configure(&other.transport, YAN_RAM_BASE,
                                        UINT32_C(256), YAN_RAM_BASE, YAN_RAM_SIZE));
    yan_transport_set_notify(&other.transport, counting_notify, &other);

    /* Two machines share the MMIO address and nothing else. */
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_IRQ_ENABLE, 1);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&machine.transport, 9));
    TEST_ASSERT_EQUAL_HEX32(9, machine.transport.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, other.transport.h2g_head);
    TEST_ASSERT_EQUAL_HEX32(0, other.transport.irq_status);

    write_machine_word(&machine, YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4, 1);
    write_machine_word(&machine, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M, 1U << YAN_MACHINE_PLIC_SOURCE_TRANSPORT);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_sample_devices(&other));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&machine.plic));
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&other.plic));

    /* A machine's transport is only reached through its own bus. */
    write_machine_word(&machine, YAN_TRANSPORT_BASE + YAN_TRANSPORT_G2H_HEAD, 3);
    TEST_ASSERT_EQUAL_HEX32(3, machine.transport.g2h_head);
    TEST_ASSERT_EQUAL_HEX32(0, other.transport.g2h_head);

    /* Destroying one machine disconnects its own window without touching the
     * other's device or RAM. */
    yan_machine_destroy(&machine);
    TEST_ASSERT_NULL(machine.bus.transport);
    TEST_ASSERT_NULL(machine.bus.ram);
    YanBus without_device = {0};
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_init(&without_device, &other.ram, YAN_RAM_BASE));
    TEST_ASSERT_EQUAL_INT(
        YAN_UNMAPPED,
        yan_bus_read(&without_device, YAN_TRANSPORT_BASE, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_read(&other.bus, YAN_TRANSPORT_BASE, 4, &value).status);
    TEST_ASSERT_EQUAL_HEX32(YAN_TRANSPORT_MAGIC, value);
    yan_machine_destroy(&other);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(identity_registers_are_constant);
    RUN_TEST(host_ready_is_derived_from_the_configuration);
    RUN_TEST(read_only_registers_ignore_writes);
    RUN_TEST(configuration_validates_placement);
    RUN_TEST(rings_start_empty_and_round_trip_bytes);
    RUN_TEST(status_reports_full_and_empty_edges);
    RUN_TEST(ring_wrap_and_byte_order);
    RUN_TEST(overrun_latches_the_first_error);
    RUN_TEST(doorbell_notifies_the_registered_host);
    RUN_TEST(irq_status_is_write_one_to_clear);
    RUN_TEST(window_errors_and_host_arguments);
    RUN_TEST(irq_line_drives_plic_source_one);
    RUN_TEST(machine_reset_keeps_the_host_configuration);
    RUN_TEST(machine_isolation_and_destroy);
    return UNITY_END();
}
