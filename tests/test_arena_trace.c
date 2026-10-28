/* Issue 78/79 support: the Host-side arena observation used by the real
 * production Guest runs. These cases drive the observer with actual YanCpu,
 * YanBus and RAM words; they do not re-run the Guest or mirror the allocation
 * formula. The selected production function is non-recursive, so a revisit of
 * its entry while a call is pending is the interrupt-resampling case. */
#include "arena_trace.h"

#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "yan/bus.h"
#include "yan/cpu.h"
#include "yan/ram.h"

#define BASE UINT32_C(0x80000000)
#define ENTRY (BASE + UINT32_C(0x100))
#define RETURN_PC (BASE + UINT32_C(0x108))
#define ARENA_ADDR (BASE + UINT32_C(0x1000))
#define START (BASE + UINT32_C(0x2004))
#define END (BASE + UINT32_C(0x4000))
#define RAM_BYTES 65536u

static YanRam ram;
static YanBus bus;
static YanCpu cpu;

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    cpu = (YanCpu){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, RAM_BYTES));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, BASE));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, ARENA_ADDR, 4, START).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, ARENA_ADDR + 4u, 4, END).status);
}

void tearDown(void)
{
    yan_ram_destroy(&ram);
}

static void arm(YanArenaTrace *trace)
{
    yan_arena_trace_configure(trace, true, true, ENTRY, START);
}

static void set_next(uint32_t next)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_bus_write(&bus, ARENA_ADDR, 4, next).status);
}

static void enter(YanArenaTrace *trace, uint32_t arena, uint32_t bytes)
{
    cpu.pc = ENTRY;
    cpu.regs[1] = RETURN_PC;
    cpu.regs[10] = arena;
    cpu.regs[11] = bytes;
    yan_arena_trace_step(trace, &cpu, &bus);
}

static void leave(YanArenaTrace *trace, uint32_t returned)
{
    cpu.pc = RETURN_PC;
    cpu.regs[10] = returned;
    yan_arena_trace_step(trace, &cpu, &bus);
}

static void test_highwater_includes_alignment_padding(void)
{
    YanArenaTrace trace;
    arm(&trace);
    enter(&trace, ARENA_ADDR, 16);
    set_next(BASE + UINT32_C(0x2020));
    leave(&trace, BASE + UINT32_C(0x2010));
    TEST_ASSERT_EQUAL_UINT64(1, trace.calls);
    TEST_ASSERT_EQUAL_UINT64(1, trace.successes);
    TEST_ASSERT_EQUAL_UINT64(0, trace.failures);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x1c), trace.highwater);
    TEST_ASSERT_EQUAL_UINT32(BASE + UINT32_C(0x2020), trace.cursor);
    TEST_ASSERT_EQUAL_STRING("ok", yan_arena_trace_state(&trace));
}

static void test_rejected_allocation_leaves_cursor_unchanged(void)
{
    YanArenaTrace trace;
    arm(&trace);
    enter(&trace, ARENA_ADDR, 32);
    leave(&trace, 0);
    TEST_ASSERT_EQUAL_UINT64(1, trace.calls);
    TEST_ASSERT_EQUAL_UINT64(0, trace.successes);
    TEST_ASSERT_EQUAL_UINT64(1, trace.failures);
    TEST_ASSERT_EQUAL_UINT32(0, trace.highwater);
    TEST_ASSERT_EQUAL_UINT32(START, trace.cursor);
    TEST_ASSERT_EQUAL_STRING("ok", yan_arena_trace_state(&trace));
}

static void test_repeated_entry_sample_does_not_double_count(void)
{
    YanArenaTrace trace;
    arm(&trace);
    enter(&trace, ARENA_ADDR, 16);
    /* A trap can sample the entry boundary before the first instruction runs;
     * the return address of the pending call must survive. */
    cpu.pc = ENTRY;
    cpu.regs[1] = RETURN_PC + 4u;
    cpu.regs[11] = 7u;
    yan_arena_trace_step(&trace, &cpu, &bus);
    TEST_ASSERT_EQUAL_UINT64(1, trace.calls);
    set_next(BASE + UINT32_C(0x2020));
    leave(&trace, BASE + UINT32_C(0x2010));
    TEST_ASSERT_EQUAL_UINT64(1, trace.calls);
    TEST_ASSERT_EQUAL_UINT64(1, trace.successes);
    TEST_ASSERT_EQUAL_STRING("ok", yan_arena_trace_state(&trace));
}

static void test_unavailable_and_unreturned_are_not_ok(void)
{
    YanArenaTrace trace;
    yan_arena_trace_configure(&trace, true, false, ENTRY, START);
    TEST_ASSERT_EQUAL_STRING("unavailable", yan_arena_trace_state(&trace));
    yan_arena_trace_finish(&trace);
    TEST_ASSERT_EQUAL_STRING("unavailable", yan_arena_trace_state(&trace));

    arm(&trace);
    enter(&trace, ARENA_ADDR, 16);
    yan_arena_trace_finish(&trace);
    TEST_ASSERT_EQUAL_STRING("incomplete", yan_arena_trace_state(&trace));
    TEST_ASSERT_TRUE(strcmp("ok", yan_arena_trace_state(&trace)) != 0);
}

static void test_unreadable_arena_address_is_an_error(void)
{
    YanArenaTrace trace;
    arm(&trace);
    enter(&trace, UINT32_C(0x90000000), 16);
    leave(&trace, BASE + UINT32_C(0x2010));
    TEST_ASSERT_EQUAL_STRING("error", yan_arena_trace_state(&trace));
}

static void test_inconsistent_returned_cursor_is_an_error(void)
{
    YanArenaTrace trace;
    arm(&trace);
    enter(&trace, ARENA_ADDR, 16);
    set_next(BASE + UINT32_C(0x2018));
    leave(&trace, BASE + UINT32_C(0x2010));
    TEST_ASSERT_EQUAL_STRING("error", yan_arena_trace_state(&trace));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_highwater_includes_alignment_padding);
    RUN_TEST(test_rejected_allocation_leaves_cursor_unchanged);
    RUN_TEST(test_repeated_entry_sample_does_not_double_count);
    RUN_TEST(test_unavailable_and_unreturned_are_not_ok);
    RUN_TEST(test_unreadable_arena_address_is_an_error);
    RUN_TEST(test_inconsistent_returned_cursor_is_an_error);
    return UNITY_END();
}
