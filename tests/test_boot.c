#include "yan/machine.h"
#include "yan/uart.h"
#include "unity.h"

/* The Guest platform views are included on purpose. Host and Guest are compiled
 * separately, so the only way to keep the two descriptions of the same device
 * map and the same trap ABI in step is to compare them in one translation unit.
 */
#include "guest_devices.h"
#include "mtrap.h"
#include "mtrap_frame.h"
/* os/platform.h is YanOS's own restatement of the UART map and of the PLIC
 * source numbers. It is a Guest header for the same reason as the two above: a
 * freestanding build cannot include include/yan, so the restatement is compared
 * here, in the one translation unit that can see both descriptions. */
#include "platform.h"

static YanMachine machine;

void setUp(void)
{
    machine = (YanMachine){0};
}

void tearDown(void)
{
    yan_machine_destroy(&machine);
}

/* The Guest header restates the device map because a freestanding Guest build
 * must not pull in the Host's own yan headers. These assertions are what makes
 * the restatement safe: a divergence fails the build's test run. */
static void guest_device_view_matches_the_host(void)
{
    TEST_ASSERT_EQUAL_HEX32(YAN_CLINT_BASE, YAN_GUEST_CLINT_BASE);
    TEST_ASSERT_EQUAL_HEX32(YAN_CLINT_SIZE, YAN_GUEST_CLINT_SIZE);
    TEST_ASSERT_EQUAL_HEX32(YAN_CLINT_MSIP, YAN_GUEST_CLINT_MSIP);
    TEST_ASSERT_EQUAL_HEX32(YAN_CLINT_MTIMECMP, YAN_GUEST_CLINT_MTIMECMP);
    TEST_ASSERT_EQUAL_HEX32(YAN_CLINT_MTIME, YAN_GUEST_CLINT_MTIME);

    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_BASE, YAN_GUEST_PLIC_BASE);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_SIZE, YAN_GUEST_PLIC_SIZE);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_PRIORITY, YAN_GUEST_PLIC_PRIORITY);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_PENDING, YAN_GUEST_PLIC_PENDING);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_ENABLE_M, YAN_GUEST_PLIC_ENABLE_M);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_THRESHOLD_M, YAN_GUEST_PLIC_THRESHOLD_M);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_CLAIM_M, YAN_GUEST_PLIC_CLAIM_M);
    TEST_ASSERT_EQUAL_HEX32(YAN_PLIC_SOURCE_MAX, YAN_GUEST_PLIC_SOURCE_MAX);

    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP, YAN_GUEST_MIE_MSIP);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MTIP, YAN_GUEST_MIE_MTIP);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, YAN_GUEST_MIE_MEIP);
    TEST_ASSERT_EQUAL_HEX32(YAN_MIE_MASK, YAN_GUEST_MIE_MASK);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MIE, YAN_GUEST_MSTATUS_MIE);

    /* The trap ABI names CSRs by number in its documentation, so the numbers
     * themselves are part of the contract the Guest boot code relies on. */
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x300), YAN_GUEST_CSR_MSTATUS);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x304), YAN_GUEST_CSR_MIE);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x305), YAN_GUEST_CSR_MTVEC);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x340), YAN_GUEST_CSR_MSCRATCH);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x341), YAN_GUEST_CSR_MEPC);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x342), YAN_GUEST_CSR_MCAUSE);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x343), YAN_GUEST_CSR_MTVAL);

    /* Every CSR the Guest ABI names is one the Host actually implements. */
    uint32_t value = 0;
    YanCpu cpu = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, 0));
    const uint32_t addresses[] = {YAN_GUEST_CSR_MSTATUS, YAN_GUEST_CSR_MIE,
                                  YAN_GUEST_CSR_MTVEC, YAN_GUEST_CSR_MSCRATCH,
                                  YAN_GUEST_CSR_MEPC, YAN_GUEST_CSR_MCAUSE,
                                  YAN_GUEST_CSR_MTVAL};
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK,
                              yan_cpu_read_csr(&cpu, addresses[i], &value));
    }
    /* The Guest ABI deliberately names no delegation or PMP CSR. */
    for (uint32_t address = UINT32_C(0x302); address <= UINT32_C(0x303); ++address) {
        TEST_ASSERT_EQUAL_INT(YAN_UNSUPPORTED_INSTRUCTION,
                              yan_cpu_read_csr(&cpu, address, &value));
    }
}

/* YanOS restates the UART map in os/platform.h exactly as the Guest headers
 * above restate CLINT and PLIC, and for the same reason. Every restated
 * constant is compared against the Host header it mirrors, including the one
 * pair whose names differ: the receive latch is YAN_UART_IRQ_RX_PENDING on the
 * Host side and YAN_OS_UART_IRQ_RX_PENDING on the Guest side. */
static void guest_uart_view_matches_the_host(void)
{
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_BASE, YAN_OS_UART_BASE);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_SIZE, YAN_OS_UART_SIZE);

    TEST_ASSERT_EQUAL_HEX32(YAN_UART_TXDATA, YAN_OS_UART_TXDATA);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_RXDATA, YAN_OS_UART_RXDATA);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS, YAN_OS_UART_STATUS);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_CONTROL, YAN_OS_UART_CONTROL);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_STATUS, YAN_OS_UART_IRQ_STATUS);

    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_TX_READY,
                            YAN_OS_UART_STATUS_TX_READY);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_RX_READY,
                            YAN_OS_UART_STATUS_RX_READY);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_STATUS_CONNECTED,
                            YAN_OS_UART_STATUS_CONNECTED);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_CONTROL_RX_IRQ_ENABLE,
                            YAN_OS_UART_CONTROL_RX_IRQ_ENABLE);
    TEST_ASSERT_EQUAL_HEX32(YAN_UART_IRQ_RX_PENDING,
                            YAN_OS_UART_IRQ_RX_PENDING);

    /* The Guest driver accesses whole words only, because the devices answer no
     * other width, so the restated offsets must stay word-aligned and distinct:
     * two registers sharing an offset would make one access answer for both. */
    const uint32_t offsets[] = {
        YAN_OS_UART_TXDATA, YAN_OS_UART_RXDATA, YAN_OS_UART_STATUS,
        YAN_OS_UART_CONTROL, YAN_OS_UART_IRQ_STATUS
    };
    const size_t count = sizeof offsets / sizeof offsets[0];
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_HEX32(0, offsets[i] % 4);
        for (size_t j = i + 1; j < count; ++j) {
            TEST_ASSERT_TRUE(offsets[i] != offsets[j]);
        }
    }
    /* Every restated register must land inside the restated window, and that
     * window must stay clear of RAM the way the CLINT and PLIC windows do. */
    TEST_ASSERT_TRUE(YAN_OS_UART_IRQ_STATUS + 4 <= YAN_OS_UART_SIZE);
    TEST_ASSERT_TRUE(YAN_OS_UART_BASE + YAN_OS_UART_SIZE <= YAN_RAM_BASE);
}

/* The platform header is where the transport and the UART are mapped onto PLIC
 * source numbers. The Guest restates those numbers and enables them in its own
 * copy of the PLIC; a drifted number would make the Guest unmask a source that
 * belongs to another device. */
static void guest_plic_sources_match_the_host(void)
{
    TEST_ASSERT_EQUAL_HEX32(YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                            YAN_OS_PLIC_SOURCE_TRANSPORT);
    TEST_ASSERT_EQUAL_HEX32(YAN_MACHINE_PLIC_SOURCE_UART,
                            YAN_OS_PLIC_SOURCE_UART);

    /* Source 0 is reserved, the two named sources are distinct, and both must
     * be numbers this PLIC can decode: a Guest that enables one of them must
     * not thereby unmask the other, nor write past the source window. */
    TEST_ASSERT_TRUE(YAN_OS_PLIC_SOURCE_TRANSPORT != 0);
    TEST_ASSERT_TRUE(YAN_OS_PLIC_SOURCE_UART != 0);
    TEST_ASSERT_TRUE(YAN_OS_PLIC_SOURCE_TRANSPORT != YAN_OS_PLIC_SOURCE_UART);
    TEST_ASSERT_TRUE(YAN_OS_PLIC_SOURCE_TRANSPORT <=
                    (uint32_t)YAN_PLIC_SOURCE_MAX);
    TEST_ASSERT_TRUE(YAN_OS_PLIC_SOURCE_UART <= (uint32_t)YAN_PLIC_SOURCE_MAX);
}

/* The trap vector's frame is an ABI: the assembly writes it and the C
 * dispatcher must not be able to observe it. Slot identity and alignment are
 * asserted here rather than left to a comment. */
static void trap_frame_is_a_consistent_abi(void)
{
    const uint32_t slots[] = {
        YAN_GUEST_TRAP_RA, YAN_GUEST_TRAP_T0, YAN_GUEST_TRAP_T1,
        YAN_GUEST_TRAP_T2, YAN_GUEST_TRAP_A0, YAN_GUEST_TRAP_A1,
        YAN_GUEST_TRAP_A2, YAN_GUEST_TRAP_A3, YAN_GUEST_TRAP_A4,
        YAN_GUEST_TRAP_A5, YAN_GUEST_TRAP_A6, YAN_GUEST_TRAP_A7,
        YAN_GUEST_TRAP_T3, YAN_GUEST_TRAP_T4, YAN_GUEST_TRAP_T5,
        YAN_GUEST_TRAP_T6, YAN_GUEST_TRAP_SP
    };
    const size_t count = sizeof slots / sizeof slots[0];
    TEST_ASSERT_EQUAL_UINT32(17, count);
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_TRUE(slots[i] < YAN_GUEST_TRAP_WORDS);
        for (size_t j = i + 1; j < count; ++j) {
            TEST_ASSERT_TRUE(slots[i] != slots[j]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(YAN_GUEST_TRAP_WORDS * 4, YAN_GUEST_TRAP_FRAME_BYTES);
    /* 16-byte alignment keeps the frame valid for any RV32 ABI call below it. */
    TEST_ASSERT_EQUAL_UINT32(0, YAN_GUEST_TRAP_FRAME_BYTES % 16);
    /* The interrupted sp is recovered as sp + the frame size, so the vector
     * must push one full frame before it stores anything. */
    TEST_ASSERT_TRUE(YAN_GUEST_TRAP_SP * 4 + 4 <= YAN_GUEST_TRAP_FRAME_BYTES);
}

/* Encodings the CPU executes below. A wrong constant would decode as an
 * unsupported instruction and turn the step into a trap, so the test fails
 * loudly rather than silently checking nothing. */
#define INSN_SW_T1_0_T0 UINT32_C(0x0062a023)     /* sw   t1, 0(t0)  */
#define INSN_SW_ZERO_0_T0 UINT32_C(0x0002a023)   /* sw  zero, 0(t0) */
#define INSN_LW_T2_0_T0 UINT32_C(0x0002a383)     /* lw   t2, 0(t0)  */
#define INSN_SW_T1_4_T0 UINT32_C(0x0062a223)     /* sw   t1, 4(t0)  */
#define INSN_LW_T2_4_T0 UINT32_C(0x0042a383)     /* lw   t2, 4(t0)  */
#define INSN_CSRW_MTVEC_T0 UINT32_C(0x30529073)  /* csrw mtvec, t0  */
#define INSN_CSRS_MSTATUS_T1 UINT32_C(0x30032073) /* csrs mstatus, t1 */

static void load_program(const uint32_t *words, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK,
                              yan_bus_write(&machine.bus, YAN_RAM_BASE + 4 * i,
                                            4, words[i]).status);
    }
}

static void step_once(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
}

static uint32_t read_reg(uint32_t index)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_reg(&machine.cpu, index, &value));
    return value;
}

/* Guest-issued MMIO: the CPU executes the store and the load itself. The Host
 * only seeds the base registers and inspects the device afterwards. */
static void guest_instructions_reach_clint_and_plic(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    const uint32_t clint_program[] = {
        INSN_SW_T1_0_T0,   /* CLINT msip <- 1 */
        INSN_LW_T2_0_T0,   /* read it back    */
        INSN_SW_ZERO_0_T0  /* clear it        */
    };
    const uint32_t plic_program[] = {
        INSN_SW_T1_4_T0,   /* PLIC priority[1] <- t1 */
        INSN_LW_T2_4_T0    /* read it back           */
    };

    load_program(clint_program, sizeof clint_program / sizeof clint_program[0]);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_cpu_write_reg(&machine.cpu, 5, YAN_GUEST_CLINT_BASE));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&machine.cpu, 6, 1));
    step_once();
    TEST_ASSERT_EQUAL_HEX32(1, machine.clint.msip);
    step_once();
    TEST_ASSERT_EQUAL_HEX32(1, read_reg(7));
    step_once();
    TEST_ASSERT_EQUAL_HEX32(0, machine.clint.msip);
    /* The whole access went through the Guest address, not a Host call. */
    TEST_ASSERT_EQUAL_HEX32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MSIP,
                            UINT32_C(0x02000000));

    /* The same encodings now address the PLIC through the Guest's base. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&machine));
    load_program(plic_program, sizeof plic_program / sizeof plic_program[0]);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_cpu_write_reg(&machine.cpu, 5, YAN_GUEST_PLIC_BASE));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&machine.cpu, 6, UINT32_C(0x5a5a)));
    step_once();
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x5a5a), machine.plic.priority[1]);
    step_once();
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x5a5a), read_reg(7));

    /* Both windows are the standard ones and sit outside the RAM mapping. */
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x02000000), YAN_GUEST_CLINT_BASE);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x0c000000), YAN_GUEST_PLIC_BASE);
    TEST_ASSERT_TRUE(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_SIZE <= YAN_RAM_BASE);
    TEST_ASSERT_TRUE(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_SIZE <= YAN_RAM_BASE);
}

/* The boot prolog's contract: reset leaves no vector and interrupts off, a
 * direct vector is installed with a CSR instruction, and the two low mode bits
 * of mtvec are not stored. */
static void boot_contract_installs_the_vector(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mtvec);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mstatus & YAN_MSTATUS_MIE);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mie);

    /* The trap frame is entered with the interrupted sp, so the vector must be
     * a word-aligned direct address: mtvec keeps bits 31:2 only. */
    const uint32_t handler = YAN_RAM_BASE + UINT32_C(0x40);
    const uint32_t program[] = {
        INSN_CSRW_MTVEC_T0,
        INSN_CSRS_MSTATUS_T1
    };
    load_program(program, sizeof program / sizeof program[0]);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_cpu_write_reg(&machine.cpu, 5, handler | UINT32_C(3)));
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_cpu_write_reg(&machine.cpu, 6, YAN_MSTATUS_MIE));
    step_once();
    step_once();
    TEST_ASSERT_EQUAL_HEX32(handler, machine.cpu.csr.mtvec);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MIE,
                            machine.cpu.csr.mstatus & YAN_MSTATUS_MIE);

    /* The Guest boot code starts from a state with no interrupt enabled. */
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mie);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mstatus & YAN_MSTATUS_MPIE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(guest_device_view_matches_the_host);
    RUN_TEST(guest_uart_view_matches_the_host);
    RUN_TEST(guest_plic_sources_match_the_host);
    RUN_TEST(trap_frame_is_a_consistent_abi);
    RUN_TEST(guest_instructions_reach_clint_and_plic);
    RUN_TEST(boot_contract_installs_the_vector);
    return UNITY_END();
}
