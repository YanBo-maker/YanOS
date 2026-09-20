#include "yan/machine.h"
#include "unity.h"

/* One hart, M-mode only: the tests below drive the device lines from the host
 * and observe the architectural state the CPU commits at an instruction
 * boundary. */

#define CSR_MSTATUS UINT32_C(0x300)
#define CSR_MTVEC UINT32_C(0x305)
#define CSR_MIE UINT32_C(0x304)
#define CSR_MIP UINT32_C(0x344)

#define ENTRY YAN_RAM_BASE
#define HANDLER (YAN_RAM_BASE + UINT32_C(0x100))

#define INSTRUCTION_ADDI_X1 UINT32_C(0x00100093) /* addi x1, x0, 1 */
#define INSTRUCTION_ADDI_X2 UINT32_C(0x00200113) /* addi x2, x0, 2 */
#define INSTRUCTION_NOP UINT32_C(0x00000013)
#define INSTRUCTION_MRET UINT32_C(0x30200073)
#define INSTRUCTION_ECALL UINT32_C(0x00000073)

#define MCAUSE_INTERRUPT UINT32_C(0x80000000)
#define MCAUSE_MSIP (MCAUSE_INTERRUPT | UINT32_C(3))
#define MCAUSE_MTIP (MCAUSE_INTERRUPT | UINT32_C(7))
#define MCAUSE_MEIP (MCAUSE_INTERRUPT | UINT32_C(11))

static YanMachine machine;
static YanMachine other;

void setUp(void)
{
    machine = (YanMachine){0};
    other = (YanMachine){0};
}

void tearDown(void)
{
    yan_machine_destroy(&machine);
    yan_machine_destroy(&other);
}

static void write_word(YanMachine *m, uint32_t address, uint32_t word)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&m->bus, address, 4, word).status);
}

static uint32_t read_word(YanMachine *m, uint32_t address)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&m->bus, address, 4, &value).status);
    return value;
}

static uint32_t read_csr(YanMachine *m, uint32_t address)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_csr(&m->cpu, address, &value));
    return value;
}

static void set_csr(YanMachine *m, uint32_t address, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&m->cpu, address, value));
}

static void bring_up(YanMachine *m)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(m));
    write_word(m, ENTRY, INSTRUCTION_ADDI_X1);
    write_word(m, ENTRY + 4, INSTRUCTION_ADDI_X2);
    write_word(m, ENTRY + 8, INSTRUCTION_NOP);
    write_word(m, HANDLER, INSTRUCTION_MRET);
    set_csr(m, CSR_MTVEC, HANDLER);
}

static void arm_msip(YanMachine *m)
{
    write_word(m, YAN_CLINT_BASE + YAN_CLINT_MSIP, 1);
}

static void clear_msip(YanMachine *m)
{
    write_word(m, YAN_CLINT_BASE + YAN_CLINT_MSIP, 0);
}

static void arm_mtip(YanMachine *m, uint64_t deadline)
{
    write_word(m, YAN_CLINT_BASE + YAN_CLINT_MTIMECMP, (uint32_t)deadline);
    write_word(m, YAN_CLINT_BASE + YAN_CLINT_MTIMECMP + 4,
               (uint32_t)(deadline >> 32));
}

static void disarm_mtip(YanMachine *m)
{
    arm_mtip(m, UINT64_MAX);
}

static uint32_t arm_meip(YanMachine *m, uint32_t source, uint32_t priority)
{
    write_word(m, YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4 * source, priority);
    write_word(m, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M, UINT32_C(1) << source);
    yan_plic_raise(&m->plic, source);
    return read_word(m, YAN_PLIC_BASE + YAN_PLIC_PENDING) & (UINT32_C(1) << source);
}

static uint32_t claim_meip(YanMachine *m)
{
    return read_word(m, YAN_PLIC_BASE + YAN_PLIC_CLAIM_M);
}

static void enable_interrupts(YanMachine *m, uint32_t mask)
{
    set_csr(m, CSR_MIE, mask);
    set_csr(m, CSR_MSTATUS, YAN_MSTATUS_MIE);
}

static void reset_interrupt_state(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mie);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mip);
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIE));
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));

    /* mie is writable but only the three machine-level bits exist. */
    set_csr(&machine, CSR_MIE, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(YAN_MIE_MASK, read_csr(&machine, CSR_MIE));
    TEST_ASSERT_EQUAL_HEX32(YAN_MIE_MASK, machine.cpu.csr.mie);
    set_csr(&machine, CSR_MIE, YAN_INTERRUPT_MEIP | 1U);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, read_csr(&machine, CSR_MIE));
    set_csr(&machine, CSR_MIE, 0);
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIE));

    /* mip is driven by the devices: writes are ignored, not stored. */
    set_csr(&machine, CSR_MIP, UINT32_MAX);
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_cpu_poll_interrupts(NULL, &machine.bus));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_cpu_poll_interrupts(&machine.cpu, NULL));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mip);

    /* A machine with no CLINT/PLIC attached reports no interrupt lines. */
    machine.cpu.csr.mip = YAN_INTERRUPT_MEIP;
    machine.bus.clint = NULL;
    machine.bus.plic = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));
}

static void poll_samples_each_device_line(void)
{
    bring_up(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));

    arm_msip(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP, read_csr(&machine, CSR_MIP));
    clear_msip(&machine);

    arm_mtip(&machine, 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MTIP, read_csr(&machine, CSR_MIP));
    disarm_mtip(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));

    TEST_ASSERT_EQUAL_HEX32(1U << 1, arm_meip(&machine, 1, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, read_csr(&machine, CSR_MIP));
    TEST_ASSERT_EQUAL_HEX32(1, claim_meip(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(0, read_csr(&machine, CSR_MIP));

    /* All three lines at once are all visible in mip. */
    arm_msip(&machine);
    arm_mtip(&machine, 0);
    TEST_ASSERT_EQUAL_HEX32(1U << 1, arm_meip(&machine, 1, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_poll_interrupts(&machine.cpu, &machine.bus));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP |
                            YAN_INTERRUPT_MEIP, read_csr(&machine, CSR_MIP));
}

static void mie_mask_limits_the_cause(void)
{
    bring_up(&machine);
    arm_msip(&machine);
    arm_mtip(&machine, 0);
    TEST_ASSERT_EQUAL_HEX32(1U << 1, arm_meip(&machine, 1, 1));

    /* All three lines are pending, but mie only enables the software one. */
    enable_interrupts(&machine, YAN_INTERRUPT_MSIP);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MSIP, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(HANDLER, machine.cpu.pc);

    /* mstatus.MIE stays set, but mie = 0 blocks every pending line. */
    enable_interrupts(&machine, 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine)); /* mret */
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.pc);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine)); /* addi x1 */
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 4, machine.cpu.pc);
    uint32_t value = 0;
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 1, &value) == YAN_OK && value == 1);

    /* Re-enabling just the software line lets it through again. The explicit
     * mstatus write above cleared MPIE, so MIE has to be set again by hand. */
    set_csr(&machine, CSR_MSTATUS, YAN_MSTATUS_MIE);
    set_csr(&machine, CSR_MIE, YAN_INTERRUPT_MSIP);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MSIP, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 4, machine.cpu.csr.mepc);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 2, &value) == YAN_OK && value == 0);
}

static void global_mie_gates_entry(void)
{
    bring_up(&machine);
    arm_msip(&machine);
    set_csr(&machine, CSR_MIE, YAN_INTERRUPT_MSIP);
    set_csr(&machine, CSR_MSTATUS, 0);

    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 4, machine.cpu.pc);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 1, &value) == YAN_OK && value == 1);

    /* Raising mstatus.MIE at the next boundary lets the same line through. */
    set_csr(&machine, CSR_MSTATUS, YAN_MSTATUS_MIE);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MSIP, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(HANDLER, machine.cpu.pc);
    /* mepc names the instruction that has not run yet. */
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 4, machine.cpu.csr.mepc);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 2, &value) == YAN_OK && value == 0);
}

static void interrupt_entry_fields_and_priority(void)
{
    bring_up(&machine);
    arm_msip(&machine);
    arm_mtip(&machine, 0);
    TEST_ASSERT_EQUAL_HEX32(1U << 1, arm_meip(&machine, 1, 1));
    enable_interrupts(&machine, YAN_MIE_MASK);

    YanCpu before = machine.cpu;
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MEIP, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.csr.mepc);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mtval);
    TEST_ASSERT_EQUAL_HEX32(HANDLER, machine.cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MPP | YAN_MSTATUS_MPIE, machine.cpu.csr.mstatus);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, machine.cpu.regs, YAN_REGISTER_COUNT);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP |
                            YAN_INTERRUPT_MEIP, read_csr(&machine, CSR_MIP));

    /* MEIP wins while it is pending; claiming it clears the line. */
    TEST_ASSERT_EQUAL_HEX32(1, claim_meip(&machine));
    set_csr(&machine, CSR_MSTATUS, YAN_MSTATUS_MIE);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MTIP, machine.cpu.csr.mcause);

    /* With the timer disarmed, MTIP drops out and the software line wins. */
    set_csr(&machine, CSR_MSTATUS, YAN_MSTATUS_MIE);
    disarm_mtip(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MSIP, machine.cpu.csr.mcause);

    /* Nothing pending any more: the next step runs the handler's mret. */
    set_csr(&machine, CSR_MSTATUS, YAN_MSTATUS_MIE);
    clear_msip(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(machine.cpu.csr.mepc, machine.cpu.pc);
}

static void mtip_boundary_through_machine_step(void)
{
    bring_up(&machine);
    arm_mtip(&machine, 3);
    enable_interrupts(&machine, YAN_INTERRUPT_MTIP);

    uint32_t value = 0;
    TEST_ASSERT_EQUAL_UINT64(0, machine.clint.mtime);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine)); /* mtime 1 */
    TEST_ASSERT_EQUAL_UINT64(1, machine.clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mcause);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 1, &value) == YAN_OK && value == 1);

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine)); /* mtime 2 */
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 2, &value) == YAN_OK && value == 2);
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 8, machine.cpu.pc);

    /* The deadline is reached exactly at mtime == mtimecmp. */
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine)); /* mtime 3 */
    TEST_ASSERT_EQUAL_UINT64(3, machine.clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MTIP, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 8, machine.cpu.csr.mepc);
    TEST_ASSERT_EQUAL_HEX32(HANDLER, machine.cpu.pc);

    disarm_mtip(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine)); /* mret */
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 8, machine.cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MPP | YAN_MSTATUS_MPIE | YAN_MSTATUS_MIE,
                            machine.cpu.csr.mstatus);

    /* Execution continues where the interrupt left it. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(ENTRY + 12, machine.cpu.pc);
}

static void mret_restores_mie_from_mpie(void)
{
    bring_up(&machine);
    arm_msip(&machine);
    enable_interrupts(&machine, YAN_INTERRUPT_MSIP);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MPP | YAN_MSTATUS_MPIE, machine.cpu.csr.mstatus);

    clear_msip(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MPP | YAN_MSTATUS_MPIE | YAN_MSTATUS_MIE,
                            machine.cpu.csr.mstatus);
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 1, &value) == YAN_OK && value == 1);

    /* When the trap was taken with MIE already clear, mret must not turn it on. */
    yan_machine_destroy(&machine);
    bring_up(&machine);
    write_word(&machine, ENTRY, INSTRUCTION_ECALL);
    set_csr(&machine, CSR_MSTATUS, 0);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(11, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MPP, machine.cpu.csr.mstatus);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.pc);
    TEST_ASSERT_EQUAL_HEX32(YAN_MSTATUS_MPP | YAN_MSTATUS_MPIE, machine.cpu.csr.mstatus);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mstatus & YAN_MSTATUS_MIE);
}

static void interrupt_precedes_a_synchronous_trap(void)
{
    bring_up(&machine);
    write_word(&machine, ENTRY, INSTRUCTION_ECALL);
    arm_msip(&machine);
    enable_interrupts(&machine, YAN_INTERRUPT_MSIP);

    /* The pending line is sampled before fetch, so the ECALL never decodes. */
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MSIP, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.csr.mepc);
    TEST_ASSERT_EQUAL_HEX32(HANDLER, machine.cpu.pc);

    /* With mie cleared the same instruction faults synchronously instead. */
    machine.cpu.pc = ENTRY;
    set_csr(&machine, CSR_MIE, 0);
    set_csr(&machine, CSR_MSTATUS, YAN_MSTATUS_MIE);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_HEX32(11, machine.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(0, machine.cpu.csr.mcause & MCAUSE_INTERRUPT);
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.csr.mepc);
}

static void machine_ticks_do_not_read_the_host_clock(void)
{
    bring_up(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&machine, 0));
    TEST_ASSERT_EQUAL_UINT64(0, machine.clint.mtime);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&machine, 100));
    TEST_ASSERT_EQUAL_UINT64(100, machine.clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(100, read_word(&machine, YAN_CLINT_BASE + YAN_CLINT_MTIME));
    TEST_ASSERT_EQUAL_HEX32(0, read_word(&machine, YAN_CLINT_BASE + YAN_CLINT_MTIME + 4));

    /* One machine step advances mtime by exactly one tick. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&machine, 5));
    TEST_ASSERT_EQUAL_UINT64(105, machine.clint.mtime);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_UINT64(106, machine.clint.mtime);

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_tick(NULL, 1));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_step(NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_tick(&other, 1));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_step(&other));
}

static void wiring_lifecycle_and_device_access(void)
{
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_TRUE(machine.bus.clint == &machine.clint);
    TEST_ASSERT_TRUE(machine.bus.plic == &machine.plic);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, machine.clint.mtimecmp);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_init(&machine));

    /* A Machine whose devices are not linked must be rejected, not trusted. */
    machine.bus.plic = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_step(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_tick(&machine, 1));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_reset(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_load_image(&machine, NULL, 0));
    machine.bus.plic = &machine.plic;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&machine, 1));

    machine.bus.clint = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_step(&machine));
    machine.bus.clint = &machine.clint;

    /* Destroying the machine unlinks and clears both devices. */
    arm_msip(&machine);
    write_word(&machine, YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4, 3);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP, yan_bus_pending_interrupts(&machine.bus));
    yan_machine_destroy(&machine);
    TEST_ASSERT_TRUE(machine.bus.clint == NULL && machine.bus.plic == NULL);
    TEST_ASSERT_EQUAL_UINT64(0, machine.clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.priority[1]);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE,
                          yan_bus_read(&machine.bus, YAN_CLINT_BASE, 4, &value).status);

    /* Re-initialisation links the devices again, at their reset values. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_TRUE(machine.bus.clint == &machine.clint);
    TEST_ASSERT_TRUE(machine.bus.plic == &machine.plic);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, machine.clint.mtimecmp);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&machine.bus, YAN_CLINT_BASE, 4, &value).status);
}

static void device_reset_is_independent_per_machine(void)
{
    bring_up(&machine);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&other));

    arm_msip(&machine);
    arm_mtip(&machine, 5);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&machine, 9));
    TEST_ASSERT_EQUAL_HEX32(1U << 1, arm_meip(&machine, 1, 3));
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP |
                            YAN_INTERRUPT_MEIP,
                            yan_bus_pending_interrupts(&machine.bus));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&other, 4));
    arm_msip(&other);

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&machine));
    /* Resetting one machine restores its own devices ... */
    TEST_ASSERT_EQUAL_UINT64(0, machine.clint.mtime);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, machine.clint.mtimecmp);
    TEST_ASSERT_EQUAL_HEX32(0, machine.clint.msip);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.pending);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.enable_m);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.threshold_m);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.in_service_m);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.priority[1]);
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));
    TEST_ASSERT_EQUAL_HEX32(ENTRY, machine.cpu.pc);
    /* ... and leaves the other machine's devices untouched. */
    TEST_ASSERT_EQUAL_UINT64(4, other.clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(1, other.clint.msip);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MSIP, yan_bus_pending_interrupts(&other.bus));

    /* A reset must not hand out a stale timer deadline either. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_tick(&machine, 1000));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&other));
    TEST_ASSERT_EQUAL_UINT64(0, other.clint.mtime);
    TEST_ASSERT_EQUAL_HEX32(0, other.clint.msip);
}

static void external_source_interface_drives_meip(void)
{
    bring_up(&machine);
    /* A source without priority, enable or threshold never raises MEIP. */
    yan_plic_raise(&machine.plic, 5);
    TEST_ASSERT_EQUAL_HEX32(1U << 5, read_word(&machine, YAN_PLIC_BASE + YAN_PLIC_PENDING));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));

    write_word(&machine, YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4 * 5, 2);
    write_word(&machine, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M, 1U << 5);
    TEST_ASSERT_EQUAL_HEX32(YAN_INTERRUPT_MEIP, yan_bus_pending_interrupts(&machine.bus));

    /* Claiming through the bus clears the line and MEIP with it. */
    TEST_ASSERT_EQUAL_HEX32(5, claim_meip(&machine));
    TEST_ASSERT_EQUAL_HEX32(0, yan_bus_pending_interrupts(&machine.bus));
    TEST_ASSERT_EQUAL_HEX32(1U << 5, machine.plic.in_service_m);
    write_word(&machine, YAN_PLIC_BASE + YAN_PLIC_CLAIM_M, 5);
    TEST_ASSERT_EQUAL_HEX32(0, machine.plic.in_service_m);

    /* A full interrupt round trip from the host-driven external source. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&other));
    write_word(&other, ENTRY, INSTRUCTION_ADDI_X1);
    write_word(&other, HANDLER, INSTRUCTION_MRET);
    set_csr(&other, CSR_MTVEC, HANDLER);
    write_word(&other, YAN_PLIC_BASE + YAN_PLIC_PRIORITY + 4 * 7, 4);
    write_word(&other, YAN_PLIC_BASE + YAN_PLIC_ENABLE_M, 1U << 7);
    enable_interrupts(&other, YAN_INTERRUPT_MEIP);
    yan_plic_raise(&other.plic, 7);
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_machine_step(&other));
    TEST_ASSERT_EQUAL_HEX32(MCAUSE_MEIP, other.cpu.csr.mcause);
    TEST_ASSERT_EQUAL_HEX32(ENTRY, other.cpu.csr.mepc);
    TEST_ASSERT_EQUAL_HEX32(HANDLER, other.cpu.pc);
    /* The handler claims and completes before returning. */
    TEST_ASSERT_EQUAL_HEX32(7, claim_meip(&other));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&other));
    TEST_ASSERT_EQUAL_HEX32(ENTRY, other.cpu.pc);
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_step(&other));
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&other.cpu, 1, &value) == YAN_OK && value == 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_interrupt_state);
    RUN_TEST(poll_samples_each_device_line);
    RUN_TEST(mie_mask_limits_the_cause);
    RUN_TEST(global_mie_gates_entry);
    RUN_TEST(interrupt_entry_fields_and_priority);
    RUN_TEST(mtip_boundary_through_machine_step);
    RUN_TEST(mret_restores_mie_from_mpie);
    RUN_TEST(interrupt_precedes_a_synchronous_trap);
    RUN_TEST(machine_ticks_do_not_read_the_host_clock);
    RUN_TEST(wiring_lifecycle_and_device_access);
    RUN_TEST(device_reset_is_independent_per_machine);
    RUN_TEST(external_source_interface_drives_meip);
    return UNITY_END();
}
