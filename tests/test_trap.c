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

static void environment_calls_and_return(void)
{
    instruction(0x00000073); check_trap(11, 0);
    instruction(0x00100073); check_trap(3, base);
    const uint32_t states[] = {0, 8, 0x80, 0x88};
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x300, states[i]));
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x341, base + 19));
        instruction(0x30200073);
        YanCpu before = cpu;
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(base + 16, cpu.pc);
        TEST_ASSERT_EQUAL_HEX32(0x1880 | ((states[i] & 0x80) != 0 ? 8 : 0), cpu.csr.mstatus);
        TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
        TEST_ASSERT_EQUAL_HEX32(before.csr.mepc, cpu.csr.mepc);
        TEST_ASSERT_EQUAL_HEX32(before.csr.mcause, cpu.csr.mcause);
        TEST_ASSERT_EQUAL_HEX32(before.csr.mtval, cpu.csr.mtval);
    }
    instruction(0x302000f3); check_trap(2, 0x302000f3);
    instruction(0x00108073); check_trap(2, 0x00108073);
    instruction(0x000000f3); check_trap(2, 0x000000f3);
}

static void csr_instruction_matrix(void)
{
    const uint32_t kinds[] = {1, 2, 3, 5, 6, 7};
    for (size_t k = 0; k < 6; ++k) {
        for (uint32_t source = 0; source < 32; ++source) {
            for (uint32_t rd = 0; rd < 32; ++rd) {
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x340, 0xa5a5));
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, source, 0xf0));
                const uint32_t operand = kinds[k] >= 5 ? source : (source == 0 ? 0 : 0xf0);
                uint32_t expected = kinds[k] % 4 == 1 ? operand :
                    (kinds[k] % 4 == 2 ? 0xa5a5 | operand : 0xa5a5 & ~operand);
                instruction(UINT32_C(0x34000073) | (kinds[k] << 12) | (source << 15) | (rd << 7));
                YanCpu before = cpu;
                if (rd != 0) { before.regs[rd] = 0xa5a5; }
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
                TEST_ASSERT_EQUAL_HEX32(base + 4, cpu.pc);
                TEST_ASSERT_EQUAL_HEX32(expected, cpu.csr.mscratch);
                TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
            }
        }
    }
}

static void csr_illegal_and_suppressed_writes(void)
{
    const uint32_t readonly_reads[] = {0xf14022f3, 0xf14032f3, 0xf14062f3, 0xf14072f3};
    for (size_t i = 0; i < 4; ++i) {
        instruction(readonly_reads[i]);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(0, cpu.regs[5]);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, 0));
    const uint32_t illegal[] = {0xf14322f3, 0xf14332f3, 0xf14012f3, 0xf14052f3,
                                0xf140e2f3, 0xf140f2f3, 0x100022f3, 0x10001073, 0x340042f3};
    for (size_t i = 0; i < sizeof illegal / sizeof illegal[0]; ++i) {
        instruction(illegal[i]); check_trap(2, illegal[i]);
    }
}

static void guest_handler_resumes_execution(void)
{
    /* Integration fixture: configure mtvec, trap, read/advance mepc, MRET. */
    const struct { size_t offset; uint32_t word; } words[] = {
        {0, 0x30531073}, {4, 0x00000073}, {8, 0x00900393},
        {32, 0x341022f3}, {36, 0x00428293}, {40, 0x34129073}, {44, 0x30200073}
    };
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x305, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x300, 8));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, base + 32));
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, words[i].offset, 4, words[i].word));
    }
    uint8_t memory[64]; memcpy(memory, ram.data, sizeof memory);
    cpu.pc = base;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(base + 32, cpu.csr.mtvec);
    check_trap(11, 0);
    TEST_ASSERT_EQUAL_HEX32(base + 4, cpu.csr.mepc);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(base + 4, cpu.regs[5]);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(base + 8, cpu.regs[5]);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(base + 8, cpu.csr.mepc);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(base + 8, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(0x1888, cpu.csr.mstatus);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(9, cpu.regs[7]);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
}

static void fence_and_unsupported_extensions(void)
{
    const uint32_t fences[] = {0x0000000f, 0x0ff0000f, 0x8330000f, 0xffff8f8f};
    for (size_t i = 0; i < 4; ++i) {
        instruction(fences[i]);
        YanCpu before = cpu;
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(base + 4, cpu.pc);
        TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
        TEST_ASSERT_EQUAL_MEMORY(&before.csr, &cpu.csr, sizeof cpu.csr);
    }
    instruction(0x0000100f); check_trap(2, 0x0000100f);
    instruction(0x047302b3); check_trap(2, 0x047302b3);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(illegal_and_target_faults);
    RUN_TEST(data_fault_causes_and_addresses);
    RUN_TEST(fetch_faults_and_nested_entry);
    RUN_TEST(interrupt_enable_stack_on_entry);
    RUN_TEST(host_errors_do_not_trap);
    RUN_TEST(environment_calls_and_return);
    RUN_TEST(csr_instruction_matrix);
    RUN_TEST(csr_illegal_and_suppressed_writes);
    RUN_TEST(guest_handler_resumes_execution);
    RUN_TEST(fence_and_unsupported_extensions);
    return UNITY_END();
}
