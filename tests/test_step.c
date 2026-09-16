#include <string.h>

#include "yan/cpu.h"
#include "unity.h"

static YanRam ram;
static YanBus bus;
static YanCpu cpu;
static const uint32_t base = UINT32_C(0x80000000);

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    cpu = (YanCpu){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 8));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, base));
}

void tearDown(void)
{
    yan_ram_destroy(&ram);
}

static void put_instruction(uint32_t word)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
    cpu.pc = bus.ram_base;
}

static void fixed_arithmetic_vectors(void)
{
    /* Fixed encodings: ADDI x5,x6,imm. Expected values are independent vectors. */
    const struct { uint32_t word, source, expected; } cases[] = {
        {0x00030293, 0x89abcdef, 0x89abcdef},
        {0x00130293, 0xffffffff, 0x00000000},
        {0xfff30293, 0x00000000, 0xffffffff},
        {0x7ff30293, 0x00000001, 0x00000800},
        {0x80030293, 0x00000000, 0xfffff800},
        {0x00130293, 0x7fffffff, 0x80000000},
        {0xfff30293, 0x80000000, 0x7fffffff}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        put_instruction(cases[i].word);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, cases[i].source));
        YanCpu before = cpu;
        uint8_t memory[8];
        memcpy(memory, ram.data, sizeof memory);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(cases[i].expected, cpu.regs[5]);
        TEST_ASSERT_EQUAL_HEX32(base + 4, cpu.pc);
        before.regs[5] = cases[i].expected;
        TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, YAN_REGISTER_COUNT);
        TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
    }
}

static void all_immediate_encodings(void)
{
    /* Encode test inputs; the oracle uses signed 64-bit arithmetic. */
    for (int32_t immediate = -2048; immediate <= 2047; ++immediate) {
        const uint32_t bits = (uint32_t)immediate & UINT32_C(0xfff);
        put_instruction((bits << 20) | UINT32_C(0x00030293));
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_MAX));
        const uint32_t expected = (uint32_t)(INT64_C(4294967295) + immediate);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(expected, cpu.regs[5]);
    }
}

static void register_fields_and_aliasing(void)
{
    for (uint32_t source = 0; source < 32; ++source) {
        for (uint32_t destination = 0; destination < 32; ++destination) {
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, base));
            for (uint32_t index = 1; index < 32; ++index) {
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, index, index * 17));
            }
            YanCpu expected = cpu;
            if (destination != 0) {
                expected.regs[destination] = expected.regs[source] + 1;
            }
            put_instruction(UINT32_C(0x00100013) | (source << 15) | (destination << 7));
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
            TEST_ASSERT_EQUAL_HEX32_ARRAY(expected.regs, cpu.regs, 32);
            TEST_ASSERT_EQUAL_HEX32(base + 4, cpu.pc);
        }
    }
}

static void sequential_steps_and_nop(void)
{
    put_instruction(UINT32_C(0x00128293)); /* ADDI x5,x5,1 */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 4, 4, UINT32_C(0x00000013)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(1, cpu.regs[5]);
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_HEX32(base + 8, cpu.pc);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(cpu.csr.mtvec, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
}

static void assert_rejected_word(uint32_t word)
{
    put_instruction(word);
    YanCpu before = cpu;
    uint8_t memory[8];
    memcpy(memory, ram.data, sizeof memory);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(cpu.csr.mtvec, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(2, cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(word, cpu.csr.mtval);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
}

static void unsupported_encodings_preserve_state(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 5, 73));
    for (uint32_t opcode = 0; opcode < 128; ++opcode) {
        if (opcode != 0x13 && opcode != 0x33 && opcode != 0x37 && opcode != 0x17 &&
            opcode != 0x63 && opcode != 0x6f && opcode != 0x67 && opcode != 0x03 &&
            opcode != 0x23 && opcode != 0x0f) {
            assert_rejected_word(UINT32_C(0x00128280) | opcode);
        }
    }
    assert_rejected_word(UINT32_C(0x02029293)); /* Reserved SLLI upper bits. */
    assert_rejected_word(0);
    assert_rejected_word(UINT32_MAX);
}

static void fetch_failures_preserve_state(void)
{
    put_instruction(UINT32_C(0x00128293));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 5, 73));
    const struct { uint32_t address; YanStatus status; } cases[] = {
        {0x80000001, YAN_UNALIGNED}, {0x80000002, YAN_UNALIGNED},
        {0x80000003, YAN_UNALIGNED}, {0x7ffffffc, YAN_UNMAPPED},
        {0x80000008, YAN_UNMAPPED}
    };
    uint8_t memory[8];
    memcpy(memory, ram.data, sizeof memory);
    YanCpu before = cpu;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        cpu.pc = cases[i].address;
        TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(cpu.csr.mtvec, cpu.pc);
        TEST_ASSERT_EQUAL_HEX32(cases[i].status == YAN_UNALIGNED ? 0 : 1, cpu.csr.mcause);
        TEST_ASSERT_EQUAL_HEX32(cases[i].address, cpu.csr.mtval);
        TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    }
    YanBus empty = {0};
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_step(NULL, &bus));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_step(&cpu, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_cpu_step(&cpu, &empty));
    TEST_ASSERT_EQUAL_HEX32(cpu.csr.mtvec, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 3));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base));
    cpu.pc = base;
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(cpu.csr.mtvec, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EACH_EQUAL_UINT8(0, ram.data, 3);
}

static void pc_wraps_at_address_space_top(void)
{
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 4));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0xfffffffc)));
    put_instruction(UINT32_C(0x00100293));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(0, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(1, cpu.regs[5]);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(0, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(1, cpu.regs[5]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(fixed_arithmetic_vectors);
    RUN_TEST(all_immediate_encodings);
    RUN_TEST(register_fields_and_aliasing);
    RUN_TEST(sequential_steps_and_nop);
    RUN_TEST(unsupported_encodings_preserve_state);
    RUN_TEST(fetch_failures_preserve_state);
    RUN_TEST(pc_wraps_at_address_space_top);
    return UNITY_END();
}
