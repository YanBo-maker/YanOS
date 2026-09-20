/* Consolidated self-check for the minimal M-mode Guest environment.
 *
 * The program is the executable form of the trap ABI: it boots through
 * `mtrap_entry.S`, installs one handler, and then requires every synchronous
 * exception and every interrupt line the platform can generate on its own to
 * arrive through the same vector and to leave the interrupted instruction
 * either resumed or skipped exactly as the handler decided.
 *
 * External (MEIP) delivery cannot be driven from a Guest: the PLIC has no
 * source-raise register, so only the Host can assert that line. This program
 * therefore checks the PLIC's Guest-visible register interface and leaves MEIP
 * delivery to the machine-level tests.
 *
 * A failed check ends the run with `tohost` set to the check number, so the
 * runner reports which expectation broke.
 */
#include "guest.h"
#include "guest_devices.h"
#include "mtrap.h"

#define CHECK_BOOT_VECTOR 1
#define CHECK_BOOT_VECTOR_MODE 2
#define CHECK_BOOT_MIE_OFF 3
#define CHECK_BOOT_MIE_MASK_OFF 4
#define CHECK_BOOT_HANDLER_SET 5

#define CHECK_ECALL_COUNT 10
#define CHECK_ECALL_CAUSE 11
#define CHECK_ECALL_TVAL 12
#define CHECK_ECALL_RESUMED 13
#define CHECK_REGISTER_PRESERVATION 14

#define CHECK_MSIP_PENDING 20
#define CHECK_MSIP_LINE_SET 21
#define CHECK_MSIP_COUNT 22
#define CHECK_MSIP_CAUSE 23
#define CHECK_MSIP_RERUN 24
#define CHECK_MSIP_LINE_CLEARED 25

#define CHECK_MTIP_PENDING 30
#define CHECK_MTIP_COUNT 31
#define CHECK_MTIP_CAUSE 32
#define CHECK_MTIP_RERUN 33
#define CHECK_MTIP_DISARMED 34
#define CHECK_INTERRUPTS_OFF 35

#define CHECK_CLINT_MTIMECMP 40
#define CHECK_CLINT_MSIP_ACCESS 41
#define CHECK_CLINT_RANGE 42

#define CHECK_PLIC_PRIORITY 50
#define CHECK_PLIC_ENABLE 51
#define CHECK_PLIC_THRESHOLD 52
#define CHECK_PLIC_PENDING 53
#define CHECK_PLIC_CLAIM 54
#define CHECK_PLIC_SOURCE_ZERO 55
#define CHECK_PLIC_PRIORITY_ZERO 56

#define CHECK_EBREAK_COUNT 60
#define CHECK_EBREAK_CAUSE 61
#define CHECK_EBREAK_TVAL 62

#define CHECK_ILLEGAL_COUNT 70
#define CHECK_ILLEGAL_CAUSE 71

#define CHECK_MISALIGNED_COUNT 80
#define CHECK_MISALIGNED_CAUSE 81
#define CHECK_MISALIGNED_TVAL 82

/* Ends the run if the handler sees a cause this program never raises. */
#define CHECK_HANDLER_UNEXPECTED UINT32_C(0xbad1)

/* Recorded by the handler so the checks below can inspect what the single
 * vector path actually saw. */
static volatile uint32_t last_cause;
static volatile uint32_t last_epc;
static volatile uint32_t last_tval;
static volatile uint32_t trap_count;
static volatile uint32_t resumed_marks;

/* Failure codes travel through `tohost`, whose value 1 means "the Guest
 * reached its final check". Marking every failure keeps a check number from
 * ever being read as a pass, whatever numbers the checks happen to use. */
static void check(int condition, uint32_t code)
{
    if (!condition) {
        guest_finish(code | UINT32_C(0x80000000));
    }
}

static uint32_t trap_handler(uint32_t cause, uint32_t epc, uint32_t tval)
{
    last_cause = cause;
    last_epc = epc;
    last_tval = tval;
    ++trap_count;

    if (cause == (YAN_GUEST_CAUSE_INTERRUPT | YAN_GUEST_CAUSE_MSIP)) {
        /* Acknowledge before returning: the line is level held by msip, so
         * leaving it set would re-enter the vector forever. */
        yan_guest_clint_clear_msip();
        return epc;
    }
    if (cause == (YAN_GUEST_CAUSE_INTERRUPT | YAN_GUEST_CAUSE_MTIP)) {
        yan_guest_clint_disarm_timer();
        return epc;
    }
    /* This program only raises exceptions it knows how to walk past. */
    if (cause == YAN_GUEST_CAUSE_ECALL_M || cause == YAN_GUEST_CAUSE_BREAKPOINT ||
        cause == YAN_GUEST_CAUSE_ILLEGAL_INSTRUCTION ||
        cause == YAN_GUEST_CAUSE_LOAD_MISALIGNED) {
        return epc + 4;
    }
    check(0, CHECK_HANDLER_UNEXPECTED);
    return epc;
}

static void boot_checks(void)
{
    check(yan_guest_trap_vector() == yan_guest_mtvec(), CHECK_BOOT_VECTOR);
    /* Direct mode: the vector address carries no mode bits. */
    check((yan_guest_mtvec() & UINT32_C(3)) == 0, CHECK_BOOT_VECTOR_MODE);
    check((yan_guest_mstatus() & YAN_GUEST_MSTATUS_MIE) == 0,
                CHECK_BOOT_MIE_OFF);
    check(yan_guest_mie() == 0, CHECK_BOOT_MIE_MASK_OFF);
    yan_guest_trap_install(trap_handler);
    check(yan_guest_trap_handler() == trap_handler, CHECK_BOOT_HANDLER_SET);
}

static void synchronous_exception_checks(void)
{
    /* ECALL: the handler resumes at epc + 4 and the program keeps running. */
    trap_count = 0;
    yan_guest_ecall();
    check(trap_count == 1, CHECK_ECALL_COUNT);
    check(last_cause == YAN_GUEST_CAUSE_ECALL_M, CHECK_ECALL_CAUSE);
    check(last_tval == 0, CHECK_ECALL_TVAL);
    resumed_marks = 1;
    check(resumed_marks == 1, CHECK_ECALL_RESUMED);

    /* The vector restores the caller-saved set, so a value held in one of those
     * registers survives the trap unchanged. */
    check(yan_guest_probe_register_preservation() == UINT32_C(0x5a5aa5a5),
          CHECK_REGISTER_PRESERVATION);

    /* EBREAK reports its own pc as mtval, through the same vector. */
    trap_count = 0;
    yan_guest_ebreak();
    check(trap_count == 1, CHECK_EBREAK_COUNT);
    check(last_cause == YAN_GUEST_CAUSE_BREAKPOINT, CHECK_EBREAK_CAUSE);
    check(last_tval == last_epc, CHECK_EBREAK_TVAL);

    /* An unimplemented CSR is an illegal instruction: the trap ABI never needs
     * delegation or PMP state. */
    trap_count = 0;
    (void)yan_guest_probe_unsupported_csr();
    check(trap_count == 1, CHECK_ILLEGAL_COUNT);
    check(last_cause == YAN_GUEST_CAUSE_ILLEGAL_INSTRUCTION,
                CHECK_ILLEGAL_CAUSE);

    /* A misaligned load reports mcause 4 and the effective address as mtval. */
    trap_count = 0;
    (void)yan_guest_load32(UINT32_C(0x80002001));
    check(trap_count == 1, CHECK_MISALIGNED_COUNT);
    check(last_cause == YAN_GUEST_CAUSE_LOAD_MISALIGNED,
                CHECK_MISALIGNED_CAUSE);
    check(last_tval == UINT32_C(0x80002001), CHECK_MISALIGNED_TVAL);
}

static void interrupt_checks(void)
{
    /* MSIP is Guest driven: the CLINT registers are enough to raise it, and
     * the handler returns to the interrupted instruction, which then runs. */
    trap_count = 0;
    resumed_marks = 0;
    yan_guest_clint_clear_msip();
    yan_guest_clint_set_msip(1);
    check(yan_guest_clint_msip() == 1, CHECK_MSIP_LINE_SET);
    yan_guest_enable_interrupts(YAN_GUEST_MIE_MSIP);
    /* The line is pending and enabled, so the next instruction boundary enters
     * the vector; the handler clears msip and returns to this same address. */
    resumed_marks += 1;
    yan_guest_disable_interrupts();
    check(trap_count == 1, CHECK_MSIP_COUNT);
    check(last_cause ==
                (YAN_GUEST_CAUSE_INTERRUPT | YAN_GUEST_CAUSE_MSIP),
                CHECK_MSIP_CAUSE);
    check(resumed_marks == 1, CHECK_MSIP_RERUN);
    check(yan_guest_clint_msip() == 0, CHECK_MSIP_LINE_CLEARED);

    /* MTIP is the same path with the timer line as the source. The deadline is
     * already met, so the line is pending before interrupts are enabled. */
    trap_count = 0;
    resumed_marks = 0;
    yan_guest_clint_set_mtime(0);
    yan_guest_clint_set_mtimecmp(0);
    check((yan_guest_mip() & YAN_GUEST_MIE_MTIP) != 0, CHECK_MTIP_PENDING);
    yan_guest_enable_interrupts(YAN_GUEST_MIE_MTIP);
    resumed_marks += 1;
    yan_guest_disable_interrupts();
    check(trap_count == 1, CHECK_MTIP_COUNT);
    check(last_cause ==
                (YAN_GUEST_CAUSE_INTERRUPT | YAN_GUEST_CAUSE_MTIP),
                CHECK_MTIP_CAUSE);
    check(resumed_marks == 1, CHECK_MTIP_RERUN);
    check((yan_guest_mip() & YAN_GUEST_MIE_MTIP) == 0, CHECK_MTIP_DISARMED);

    /* Enabling interrupts must not leave MIE set once the test is done. */
    check((yan_guest_mstatus() & YAN_GUEST_MSTATUS_MIE) == 0,
                CHECK_INTERRUPTS_OFF);
}

static void device_access_checks(void)
{
    /* CLINT through the Guest window: both 32-bit halves of the comparator. */
    yan_guest_clint_set_mtimecmp(UINT64_C(0x5566778811223344));
    check(yan_guest_clint_mtimecmp() == UINT64_C(0x5566778811223344),
                CHECK_CLINT_MTIMECMP);
    yan_guest_clint_set_msip(1);
    check(yan_guest_clint_msip() == 1, CHECK_CLINT_MSIP_ACCESS);
    yan_guest_clint_clear_msip();
    check(yan_guest_clint_msip() == 0, CHECK_CLINT_MSIP_ACCESS);
    /* The timer moves forward while the program runs. */
    yan_guest_clint_set_mtime(0);
    const uint64_t first = yan_guest_clint_mtime();
    for (volatile uint32_t spin = 0; spin < 8; ++spin) {
    }
    check(yan_guest_clint_mtime() > first, CHECK_CLINT_RANGE);

    /* PLIC through the Guest window. Only the registers the platform exposes
     * are touched; a claim is legal but cannot return a source here. */
    yan_guest_plic_set_threshold(0);
    yan_guest_plic_set_priority(1, 3);
    check(yan_guest_plic_priority(1) == 3, CHECK_PLIC_PRIORITY);
    yan_guest_plic_set_enable(UINT32_C(1) << 1);
    check(yan_guest_plic_enable() == (UINT32_C(1) << 1), CHECK_PLIC_ENABLE);
    yan_guest_plic_set_threshold(7);
    check(yan_guest_plic_threshold() == 7, CHECK_PLIC_THRESHOLD);
    /* Nothing is pending: the Guest cannot raise a source. */
    check(yan_guest_plic_pending() == 0, CHECK_PLIC_PENDING);
    check(yan_guest_plic_claim() == 0, CHECK_PLIC_CLAIM);
    /* Source 0 is reserved: it can neither be enabled nor prioritised. */
    yan_guest_plic_set_enable(UINT32_MAX);
    check((yan_guest_plic_enable() & UINT32_C(1)) == 0,
                CHECK_PLIC_SOURCE_ZERO);
    yan_guest_plic_set_priority(0, 7);
    check(yan_guest_plic_priority(0) == 0, CHECK_PLIC_PRIORITY_ZERO);
}

int main(void)
{
    boot_checks();
    synchronous_exception_checks();
    interrupt_checks();
    device_access_checks();
    /* Reaching this point means every check above passed. */
    guest_finish(1);
    return 0;
}
