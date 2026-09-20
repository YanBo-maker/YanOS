#include "yan/bus.h"
#include "yan/interrupt.h"
#include "unity.h"

static YanRam ram;
static YanBus bus;
static YanPlic plic;

#define PRIORITY_OF(source) (YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4U * (source))

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    plic = (YanPlic){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 64));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    bus.plic = &plic;
    yan_plic_reset(&plic);
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

static void enable_sources(uint32_t mask)
{
    write_word(YAN_PLIC_BASE + YAN_PLIC_ENABLE_M, mask);
}

static void set_priority(uint32_t source, uint32_t value)
{
    write_word(PRIORITY_OF(source), value);
}

static void reset_values(void)
{
    for (uint32_t source = 0; source < YAN_PLIC_SOURCE_COUNT; ++source) {
        TEST_ASSERT_EQUAL_HEX32(0, plic.priority[source]);
        TEST_ASSERT_EQUAL_HEX32(0, read_word(PRIORITY_OF(source)));
    }
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_PENDING));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_ENABLE_M));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
}

static void priority_registers_round_trip(void)
{
    uint32_t value = 0;
    set_priority(1, 3);
    TEST_ASSERT_EQUAL_HEX32(3, plic.priority[1]);
    TEST_ASSERT_EQUAL_HEX32(3, read_word(PRIORITY_OF(1)));

    set_priority(YAN_PLIC_SOURCE_MAX, UINT32_C(0xdeadbeef));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xdeadbeef),
                            read_word(PRIORITY_OF(YAN_PLIC_SOURCE_MAX)));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xdeadbeef),
                            plic.priority[YAN_PLIC_SOURCE_MAX]);

    /* Source 0 is reserved: its priority slot exists but never takes a value. */
    set_priority(0, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(0, plic.priority[0]);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(PRIORITY_OF(0)));

    /* Past the last priority word the PLIC window has nothing implemented. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_plic_read(&plic, PRIORITY_OF(YAN_PLIC_SOURCE_COUNT), &value));
}

static void pending_is_set_by_the_gateway_and_read_only(void)
{
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_PENDING));
    yan_plic_set_level(&plic, 1, true);
    TEST_ASSERT_EQUAL_HEX32(2, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(2, read_word(YAN_PLIC_BASE + YAN_PLIC_PENDING));
    yan_plic_set_level(&plic, YAN_PLIC_SOURCE_MAX, true);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x80000002),
                            read_word(YAN_PLIC_BASE + YAN_PLIC_PENDING));

    /* The pending register is hardware driven; writes must not clear it. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_PENDING, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x80000002), plic.pending);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x80000002),
                            read_word(YAN_PLIC_BASE + YAN_PLIC_PENDING));
}

static void enable_is_a_mask_with_source_zero_reserved(void)
{
    enable_sources(UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xfffffffe), plic.enable_m);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xfffffffe),
                            read_word(YAN_PLIC_BASE + YAN_PLIC_ENABLE_M));
    enable_sources(0);
    TEST_ASSERT_EQUAL_HEX32(0, plic.enable_m);
}

static void threshold_round_trip(void)
{
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 7);
    TEST_ASSERT_EQUAL_HEX32(7, plic.threshold_m);
    TEST_ASSERT_EQUAL_HEX32(7, read_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M));
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 0);
    TEST_ASSERT_EQUAL_HEX32(0, plic.threshold_m);
}

static void meip_needs_pending_enable_and_priority(void)
{
    set_priority(1, 1);
    /* Priority alone never asserts MEIP: the source is not pending yet. */
    enable_sources(1U << 1);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));

    yan_plic_set_level(&plic, 1, true);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_bus_pending_interrupts(&bus));

    /* ... and not pending + priority without enable. */
    enable_sources(0);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    enable_sources(1U << 1);

    /* The comparison is strictly greater, so priority == threshold blocks. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 1);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));

    /* Priority zero means "never interrupts". */
    set_priority(1, 0);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    set_priority(1, 1);

    /* Disabling one source leaves the other sources visible. */
    yan_plic_set_level(&plic, 2, true);
    set_priority(2, 5);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));
    enable_sources(1U << 2);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));
    enable_sources(0);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
}

static void claim_clears_pending_and_marks_in_service(void)
{
    set_priority(1, 2);
    enable_sources(1U << 1);
    yan_plic_set_level(&plic, 1, true);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));

    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(1U << 1, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_PENDING));

    /* Nothing is left to claim, so a second claim returns zero. */
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));

    /* Completing the handler releases the in-service bit. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 1);
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);

    /* Completing an unknown or reserved source is ignored, not a fault. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 0);
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 32);
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);
}

static void claim_arbitration(void)
{
    /* Equal priorities resolve to the lowest source id. */
    set_priority(1, 5);
    set_priority(2, 5);
    set_priority(3, 5);
    enable_sources((1U << 1) | (1U << 2) | (1U << 3));
    yan_plic_set_level(&plic, 3, true);
    yan_plic_set_level(&plic, 2, true);
    yan_plic_set_level(&plic, 1, true);
    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(2, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(3, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));

    /* Those three sources are still in service with their lines asserted. A
     * level-triggered source stays closed until it is completed, so release the
     * lines and complete the handlers before reusing them. */
    for (uint32_t source = 1; source <= 3; ++source) {
        yan_plic_set_level(&plic, source, false);
        write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, source);
    }
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);

    /* A higher priority wins regardless of the source id. */
    set_priority(2, 9);
    yan_plic_set_level(&plic, 2, true);
    yan_plic_set_level(&plic, 3, true);
    TEST_ASSERT_EQUAL_HEX32(2, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));

    /* Sources at or below the threshold stay invisible to claim. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 5);
    yan_plic_set_level(&plic, 1, true);
    yan_plic_set_level(&plic, 3, true);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));

    /* Lifting the threshold above every priority blocks everything. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 9);
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&bus));
}

/* Level-triggered gateway as specified in docs/specs/0016. This replaces the
 * earlier "gateway_is_not_latched" case, which asserted the opposite: the
 * gateway then had no level latch, so a source asserted while it was already in
 * service re-pended at once and "complete" re-sampled nothing. The inversion
 * keeps the claim just as explicit: the gate is now held closed from claim
 * until complete, and complete is what re-samples the line. */
static void gateway_holds_a_claimed_source_closed(void)
{
    set_priority(4, 3);
    enable_sources(1U << 4);
    yan_plic_set_level(&plic, 4, true);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));

    TEST_ASSERT_EQUAL_HEX32(4, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(1U << 4, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));

    /* The device keeps asking for service. While the source is in service the
     * gateway stays closed however often the line is driven. */
    for (int repeat = 0; repeat < 4; ++repeat) {
        yan_plic_set_level(&plic, 4, true);
        TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
        TEST_ASSERT_EQUAL_HEX32(1U << 4, plic.in_service_m);
        TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
        TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&bus));
    }

    /* Completion re-samples the line. It is still asserted, so the request is
     * raised again rather than lost. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 4);
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(1U << 4, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));

    /* Once the device stops asking, completing leaves the source quiet. */
    TEST_ASSERT_EQUAL_HEX32(4, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    yan_plic_set_level(&plic, 4, false);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 4);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
}

/* A request that has not been claimed is withdrawn when the line drops. */
static void deassert_withdraws_an_unclaimed_request(void)
{
    set_priority(3, 2);
    enable_sources(1U << 3);
    yan_plic_set_level(&plic, 3, true);
    TEST_ASSERT_EQUAL_HEX32(1U << 3, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_plic_pending(&plic));

    yan_plic_set_level(&plic, 3, false);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
}

/* A source held in service is independent of the other sources. */
static void an_in_service_source_does_not_block_others(void)
{
    set_priority(1, 2);
    set_priority(2, 2);
    enable_sources((1U << 1) | (1U << 2));
    yan_plic_set_level(&plic, 1, true);
    yan_plic_set_level(&plic, 2, true);

    TEST_ASSERT_EQUAL_HEX32(1, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    /* Source 1 keeps its line asserted for the whole handler, which must not
     * re-pend it and must not hide source 2. */
    yan_plic_set_level(&plic, 1, true);
    TEST_ASSERT_EQUAL_HEX32(1U << 1, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(1U << 2, plic.pending);
    TEST_ASSERT_EQUAL_HEX32(2, read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));

    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 1);
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 2);
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);
    /* Both lines are still asserted, so both requests are back. */
    TEST_ASSERT_EQUAL_HEX32((1U << 1) | (1U << 2), plic.pending);
}

/* An out-of-range completion must not disturb any live source. The earlier
 * case wrote the same bad ids while nothing was in service, which observes
 * nothing: a completion can only do damage if a source is actually claimed.
 * This keeps source 31 claimed with its line asserted, so a completion that
 * masks its id instead of rejecting it (for example `id & 31`, which maps
 * UINT32_MAX onto 31) would clear the in-service bit and re-pend the source. */
static void out_of_range_completion_cannot_disturb_a_live_source(void)
{
    set_priority(YAN_PLIC_SOURCE_MAX, 4);
    enable_sources(UINT32_C(1) << YAN_PLIC_SOURCE_MAX);
    yan_plic_set_level(&plic, YAN_PLIC_SOURCE_MAX, true);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_SOURCE_MAX,
                            read_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M));
    TEST_ASSERT_EQUAL_HEX32(1U << YAN_PLIC_SOURCE_MAX, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);

    const uint32_t rejected[] = {0,           YAN_PLIC_SOURCE_COUNT, 33,
                                 63,          UINT32_MAX};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, rejected[i]);
        TEST_ASSERT_EQUAL_HEX32(1U << YAN_PLIC_SOURCE_MAX, plic.in_service_m);
        TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    }

    /* The live source is still completable afterwards, and its line is still
     * asserted, so completion re-pends it. */
    write_word(YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, YAN_PLIC_SOURCE_MAX);
    TEST_ASSERT_EQUAL_HEX32(0, plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(1U << YAN_PLIC_SOURCE_MAX, plic.pending);
}

static void source_zero_is_reserved(void)
{
    yan_plic_set_level(&plic, 0, true);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);
    yan_plic_set_level(&plic, YAN_PLIC_SOURCE_COUNT, true);
    yan_plic_set_level(&plic, UINT32_MAX, true);
    TEST_ASSERT_EQUAL_HEX32(0, plic.pending);

    set_priority(0, UINT32_MAX);
    enable_sources(UINT32_MAX);
    write_word(YAN_PLIC_BASE + YAN_PLIC_THRESHOLD_M, 0);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(&plic));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xfffffffe),
                            read_word(YAN_PLIC_BASE + YAN_PLIC_ENABLE_M));
}

static void offset_and_argument_errors(void)
{
    uint32_t value = UINT32_C(0x12345678);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_read(&bus, YAN_PLIC_BASE + 2, 4, &value).status);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x12345678), value);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED,
                          yan_bus_write(&bus, YAN_PLIC_BASE + 2, 4, 0).status);

    /* Priority space is 32 words; offsets past it are not implemented. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, PRIORITY_OF(YAN_PLIC_SOURCE_COUNT), 4,
                                       &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_plic_read(&plic, YAN_PLIC_PENDING + 4, &value));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_plic_write(&plic, YAN_PLIC_THRESHOLD_M + 8, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_plic_read(&plic, YAN_PLIC_SIZE, &value));

    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_PLIC_BASE, 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&bus, YAN_PLIC_BASE, 2, 0).status);

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_plic_read(NULL, 0, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_plic_read(&plic, 0, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_plic_write(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_bus_read(&bus, YAN_PLIC_BASE, 4, NULL).status);
    TEST_ASSERT_EQUAL_HEX32(0, yan_plic_pending(NULL));
    yan_plic_set_level(NULL, 1, true);
    yan_plic_reset(NULL);
}

static void region_bounds_and_absent_device(void)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_PLIC_BASE + YAN_PLIC_SIZE, 4,
                                       &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&bus, YAN_PLIC_BASE - 4, 4, &value).status);

    /* The window is a data mapping: instruction fetch keeps the RAM-only rule. */
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_fetch32(&bus, YAN_PLIC_BASE, &value).status);

    YanBus plain = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&plain, &ram, UINT32_C(0x80000000)));
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_read(&plain, YAN_PLIC_BASE, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_bus_write(&plain, YAN_PLIC_BASE, 4, 0).status);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&plain));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_values);
    RUN_TEST(priority_registers_round_trip);
    RUN_TEST(pending_is_set_by_the_gateway_and_read_only);
    RUN_TEST(enable_is_a_mask_with_source_zero_reserved);
    RUN_TEST(threshold_round_trip);
    RUN_TEST(meip_needs_pending_enable_and_priority);
    RUN_TEST(claim_clears_pending_and_marks_in_service);
    RUN_TEST(claim_arbitration);
    RUN_TEST(gateway_holds_a_claimed_source_closed);
    RUN_TEST(deassert_withdraws_an_unclaimed_request);
    RUN_TEST(an_in_service_source_does_not_block_others);
    RUN_TEST(out_of_range_completion_cannot_disturb_a_live_source);
    RUN_TEST(source_zero_is_reserved);
    RUN_TEST(offset_and_argument_errors);
    RUN_TEST(region_bounds_and_absent_device);
    return UNITY_END();
}
