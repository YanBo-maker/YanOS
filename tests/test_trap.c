#include <string.h>
#include "yan/cpu.h"
#include "unity.h"

static YanRam ram;
static YanBus bus;
static YanCpu cpu;
static const uint32_t base = UINT32_C(0x80000000);

void setUp(void)
{
    ram = (YanRam){0}; bus = (YanBus){0}; cpu = (YanCpu){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 64));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, base));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x305, base + 32));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x340, 123));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 5, 73));
}
void tearDown(void) { yan_ram_destroy(&ram); }

static void instruction(uint32_t word)
{
    cpu.pc = base;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
}

static void check_trap(uint32_t cause, uint32_t tval)
{
    const YanCpu before = cpu;
    uint8_t memory[64]; memcpy(memory, ram.data, ram.size);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(before.csr.mtvec, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(before.pc & UINT32_C(0xfffffffc), cpu.csr.mepc);
    TEST_ASSERT_EQUAL_HEX32(cause, cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(tval, cpu.csr.mtval);
    TEST_ASSERT_EQUAL_HEX32(0x1800 | ((before.csr.mstatus & 8) != 0 ? 0x80 : 0), cpu.csr.mstatus);
    TEST_ASSERT_EQUAL_HEX32(before.csr.mscratch, cpu.csr.mscratch);
    TEST_ASSERT_EQUAL_HEX32(before.csr.mtvec, cpu.csr.mtvec);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, ram.size);
}

static void illegal_and_target_faults(void)
{
    instruction(0xffffffff); check_trap(2, 0xffffffff);
    instruction(0x002002ef); check_trap(0, base + 2);
    instruction(0x00000163); check_trap(0, base + 2);
    instruction(0x003002e7); check_trap(0, 2);
    instruction(0x00002063); check_trap(2, 0x00002063);
}

static void data_fault_causes_and_addresses(void)
{
    const uint32_t words[] = {0x00032283, 0x00732023};
    for (size_t store = 0; store < 2; ++store) {
        instruction(words[store]);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, base + 17));
        check_trap(store ? 6 : 4, base + 17);
        instruction(words[store]);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, base + 64));
        check_trap(store ? 7 : 5, base + 64);
    }
    instruction(0x00032003); check_trap(5, base + 64); /* Load to x0 still faults. */
    instruction(0x00033283); check_trap(2, 0x00033283); /* Decode before data access. */
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 63));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, base + 60));
    instruction(0x00032283); check_trap(5, base + 60);
    instruction(0x00732023); check_trap(7, base + 60);
}

static void fetch_faults_and_nested_entry(void)
{
    cpu.pc = base + 1; check_trap(0, base + 1);
    cpu.pc = base + 64; check_trap(1, base + 64);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x305, 0));
    instruction(0xffffffff); check_trap(2, 0xffffffff);
    check_trap(1, 0);
    check_trap(1, 0);
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 63));
    cpu.pc = base + 60; check_trap(1, base + 60);
}

static void interrupt_enable_stack_on_entry(void)
{
    const uint32_t states[] = {0, 8, 0x80, 0x88};
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x300, states[i]));
        instruction(0xffffffff); check_trap(2, 0xffffffff);
        instruction(0xffffffff); check_trap(2, 0xffffffff);
    }
}

static void host_errors_do_not_trap(void)
{
    instruction(0xffffffff);
    YanCpu before = cpu;
    YanBus empty = {0};
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_step(NULL, &bus));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_step(&cpu, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_cpu_step(&cpu, &empty));
    TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
    TEST_ASSERT_EQUAL_MEMORY(&before.csr, &cpu.csr, sizeof cpu.csr);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(illegal_and_target_faults);
    RUN_TEST(data_fault_causes_and_addresses);
    RUN_TEST(fetch_faults_and_nested_entry);
    RUN_TEST(interrupt_enable_stack_on_entry);
    RUN_TEST(host_errors_do_not_trap);
    return UNITY_END();
}
