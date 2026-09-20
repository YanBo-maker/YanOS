#include <string.h>

#include "yan/bus.h"
#include "yan/interrupt.h"
#include "unity.h"

/* Small RAM plus the CLINT so the MMIO dispatch is exercised without a whole
 * Machine; the Machine-level wiring is covered by test_interrupt. */
static YanRam ram;
static YanBus bus;
static YanClint clint;

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    clint = (YanClint){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 64));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    bus.clint = &clint;
    yan_clint_reset(&clint);
}

void tearDown(void)
{
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

static void reset_values(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, clint.msip);
    TEST_ASSERT_EQUAL_UINT64(0, clint.mtime);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, clint.mtimecmp);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_CLINT_BASE + YAN_CLINT_MSIP));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX,
                            read_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX,
                            read_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4));
}

static void msip_keeps_only_bit_zero(void)
{
    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, 1);
    TEST_ASSERT_EQUAL_UINT32(1, clint.msip);
    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_CLINT_BASE + YAN_CLINT_MSIP));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP, yan_clint_pending(&clint));

    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(1, clint.msip);
    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_CLINT_BASE + YAN_CLINT_MSIP));

    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, UINT32_C(0xfffffffe));
    TEST_ASSERT_EQUAL_UINT32(0, clint.msip);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));
}

static void mtimecmp_halves_are_independent(void)
{
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, UINT32_C(0x11223344));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0xffffffff11223344), clint.mtimecmp);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, UINT32_C(0x55667788));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x5566778811223344), clint.mtimecmp);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x11223344),
                            read_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x55667788),
                            read_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4));

    /* Writing the high half leaves the low half alone, and vice versa. */
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x11223344), clint.mtimecmp);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, 0);
    TEST_ASSERT_EQUAL_UINT64(0, clint.mtimecmp);
}

static void mtime_halves_and_tick(void)
{
    yan_clint_tick(&clint, 5);
    TEST_ASSERT_EQUAL_UINT64(5, clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(5, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4));

    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4, 1);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x100000005), clint.mtime);
    yan_clint_tick(&clint, 0);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x100000005), clint.mtime);
    yan_clint_tick(&clint, 1);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x100000006), clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(6, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME));
    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4));

    /*
     * No host clock is involved, so the timeline is exactly the tick sequence.
     * Zero-filling the counter proves the tick, not the wall clock, drives it.
     */
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIME, 0);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4, 0);
    TEST_ASSERT_EQUAL_UINT64(0, clint.mtime);
    yan_clint_tick(&clint, 3);
    TEST_ASSERT_EQUAL_UINT64(3, clint.mtime);
}

static void mtimecmp_boundary_triggers_at_equality(void)
{
    /* The reset comparator sits at UINT64_MAX so MTIP stays low out of reset. */
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, 10);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));

    yan_clint_tick(&clint, 9);
    TEST_ASSERT_EQUAL_UINT64(9, clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));

    yan_clint_tick(&clint, 1);
    TEST_ASSERT_EQUAL_UINT64(10, clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MTIP, yan_clint_pending(&clint));

    yan_clint_tick(&clint, 1000);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MTIP, yan_clint_pending(&clint));

    /* Raising the comparator above mtime clears MTIP again. */
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, UINT32_MAX);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, clint.mtimecmp);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));

    /* A deadline of mtime + 1 is still not reached. */
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, clint.mtime + 1);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));
    yan_clint_tick(&clint, 1);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MTIP, yan_clint_pending(&clint));
}

static void mtimecmp_zero_triggers_immediately(void)
{
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, 0);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MTIP, yan_clint_pending(&clint));
}

static void mtime_wraps_are_defined(void)
{
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIME, UINT32_MAX);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, clint.mtime);
    yan_clint_tick(&clint, 1);
    TEST_ASSERT_EQUAL_UINT64(0, clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_CLINT_BASE + YAN_CLINT_MTIME + 4));
}

static void pending_combines_msip_and_mtip(void)
{
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, 3);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    yan_clint_tick(&clint, 3);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, 1);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP,
                            yan_clint_pending(&clint));
    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, 0);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, UINT32_MAX);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));
}

static void offset_and_argument_errors(void)
{
    uint32_t value = UINT32_C(0x5a5a5a5a);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_read(&bus, YAN_CLINT_BASE + 2, 4, &value).status);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x5a5a5a5a), value);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_write(&bus, YAN_CLINT_BASE + 2, 4, 0).status);

    /* Inside the CLINT window but no register is implemented there. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_CLINT_BASE + 0x100, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, YAN_CLINT_BASE + 0x100, 4, 0).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_clint_read(&clint, 0x100, &value));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_clint_write(&clint, 0x100, 0));

    /* The device answers word accesses only; other widths keep the old path. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_CLINT_BASE, 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, YAN_CLINT_BASE, 2, 0).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_WIDTH,
                          yan_bus_read(&bus, YAN_CLINT_BASE, 3, &value).status);

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_clint_read(NULL, 0, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_clint_read(&clint, 0, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_clint_write(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_bus_read(&bus, YAN_CLINT_BASE, 4, NULL).status);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(NULL));
    yan_clint_reset(NULL);
    yan_clint_tick(NULL, 5);
}

static void region_bounds_and_absent_device(void)
{
    /* Just outside the CLINT window the address is no longer claimed by it. */
    const uint32_t outside = YAN_CLINT_BASE + YAN_CLINT_SIZE;
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_bus_read(&bus, outside, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_CLINT_BASE - 4, 4, &value).status);

    /* The window is a data mapping: instruction fetch keeps the RAM-only rule. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_fetch32(&bus, YAN_CLINT_BASE, &value).status);

    /* A bus without a CLINT must not claim the CLINT addresses either. */
    YanBus plain = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&plain, &ram, UINT32_C(0x80000000)));
    TEST_ASSERT_TRUE(plain.clint == NULL && plain.plic == NULL);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&plain, YAN_CLINT_BASE, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&plain, YAN_CLINT_BASE, 4, 0).status);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&plain));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(NULL));

    /* RAM keeps working exactly as before next to the device window. */
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, UINT32_C(0x80000000), 4,
                                        UINT32_C(0xcafef00d)).status);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xcafef00d),
                            read_word(UINT32_C(0x80000000)));
}

static void bus_pending_aggregates_devices(void)
{
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&bus));
    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, 1);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP, yan_bus_pending_interrupts(&bus));
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, 0);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP,
                            yan_bus_pending_interrupts(&bus));
}

static void reset_restores_the_comparator(void)
{
    write_word(YAN_CLINT_BASE + YAN_CLINT_MSIP, 1);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, 1);
    write_word(YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4, 0);
    yan_clint_tick(&clint, 7);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP,
                            yan_clint_pending(&clint));
    yan_clint_reset(&clint);
    TEST_ASSERT_EQUAL_HEX32(0, yan_clint_pending(&clint));
    TEST_ASSERT_EQUAL_UINT64(0, clint.mtime);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, clint.mtimecmp);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_values);
    RUN_TEST(msip_keeps_only_bit_zero);
    RUN_TEST(mtimecmp_halves_are_independent);
    RUN_TEST(mtime_halves_and_tick);
    RUN_TEST(mtimecmp_boundary_triggers_at_equality);
    RUN_TEST(mtimecmp_zero_triggers_immediately);
    RUN_TEST(mtime_wraps_are_defined);
    RUN_TEST(pending_combines_msip_and_mtip);
    RUN_TEST(offset_and_argument_errors);
    RUN_TEST(region_bounds_and_absent_device);
    RUN_TEST(bus_pending_aggregates_devices);
    RUN_TEST(reset_restores_the_comparator);
    return UNITY_END();
}
