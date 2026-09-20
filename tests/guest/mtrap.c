#include "mtrap.h"

#include <stddef.h>

#include "guest.h"

/* Single installed handler: the platform has one M-mode entry and no nesting,
 * so a one-deep table is the whole requirement. The default ends the run
 * loudly rather than returning to an address the Guest never chose. */
static yan_guest_trap_fn installed_handler = yan_guest_trap_panic;

uint32_t yan_guest_trap_vector(void)
{
    return (uint32_t)(uintptr_t)__yan_guest_trap_vector;
}

/* The CSR names below resolve to the implemented machine-mode CSRs. A Guest
 * build has no reason to use a generic CSR-by-number helper, and avoiding one
 * keeps every access a compile-time-checked named CSR. */

static uint32_t read_mstatus(void)
{
    uint32_t value;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(value));
    return value;
}

static uint32_t read_mie(void)
{
    uint32_t value;
    __asm__ volatile ("csrr %0, mie" : "=r"(value));
    return value;
}

static uint32_t read_mip(void)
{
    uint32_t value;
    __asm__ volatile ("csrr %0, mip" : "=r"(value));
    return value;
}

static uint32_t read_mtvec(void)
{
    uint32_t value;
    __asm__ volatile ("csrr %0, mtvec" : "=r"(value));
    return value;
}

uint32_t yan_guest_mstatus(void)
{
    return read_mstatus();
}

uint32_t yan_guest_mie(void)
{
    return read_mie();
}

uint32_t yan_guest_mip(void)
{
    return read_mip();
}

uint32_t yan_guest_mtvec(void)
{
    return read_mtvec();
}

void yan_guest_boot(void)
{
    const uint32_t vector = yan_guest_trap_vector();
    /* Install the vector first: after this point a fault is reportable instead
     * of jumping to an uninitialised mtvec. */
    __asm__ volatile ("csrw mtvec, %0" ::"r"(vector));
    /* Interrupts start disabled, with no line enabled. */
    __asm__ volatile ("csrw mstatus, %0" ::"r"(UINT32_C(0)));
    __asm__ volatile ("csrw mie, %0" ::"r"(UINT32_C(0)));
}

void yan_guest_trap_install(yan_guest_trap_fn handler)
{
    installed_handler = handler == NULL ? yan_guest_trap_panic : handler;
}

yan_guest_trap_fn yan_guest_trap_handler(void)
{
    return installed_handler;
}

/* The vector has already saved the caller-saved registers and read the trap
 * CSRs, so this function is ordinary C. It exists as a seam: acknowledging an
 * interrupt and choosing the resume pc stay the handler's decision, while the
 * assembly keeps only the frame and the mret. */
uint32_t yan_guest_trap_dispatch(uint32_t cause, uint32_t epc, uint32_t tval)
{
    return installed_handler(cause, epc, tval);
}

uint32_t yan_guest_trap_panic(uint32_t cause, uint32_t epc, uint32_t tval)
{
    (void)epc;
    (void)tval;
    /* guest_finish() does not return; the value it writes is the failure code
     * the runner reports. */
    guest_finish(YAN_GUEST_TRAP_PANIC | (cause & UINT32_C(0x7f)));
    return 0;
}

void yan_guest_enable_interrupts(uint32_t mask)
{
    /* Configure the lines before the global enable, so no line can be taken
     * with a stale mie. */
    __asm__ volatile ("csrs mie, %0" ::"r"(mask & YAN_GUEST_MIE_MASK));
    __asm__ volatile ("csrs mstatus, %0" ::"r"(YAN_GUEST_MSTATUS_MIE));
}

void yan_guest_disable_interrupts(void)
{
    __asm__ volatile ("csrc mstatus, %0" ::"r"(YAN_GUEST_MSTATUS_MIE));
}

void yan_guest_ecall(void)
{
    __asm__ volatile ("ecall");
}

void yan_guest_ebreak(void)
{
    __asm__ volatile ("ebreak");
}

uint32_t yan_guest_probe_register_preservation(void)
{
    /* a4 is held live across the trap on purpose. The frame exists to restore
     * exactly this register class, so an aliased or missized slot shows up as a
     * changed value rather than as a silent ABI violation. The handler walks
     * past the ECALL, so the read after it sees the restored value. */
    register uint32_t sentinel __asm__("a4") = UINT32_C(0x5a5aa5a5);
    __asm__ volatile ("ecall" : "+r"(sentinel) : : "memory");
    return sentinel;
}

uint32_t yan_guest_probe_unsupported_csr(void)
{
    /* medeleg (0x302) is outside the implemented set on purpose: the read must
     * raise an illegal-instruction exception, which is itself the statement
     * that the trap ABI needs no delegation state. */
    uint32_t value;
    __asm__ volatile ("csrr %0, 0x302" : "=r"(value));
    return value;
}

uint32_t yan_guest_load32(uint32_t address)
{
    /* volatile so the access cannot be folded away: the self-check depends on
     * the load actually reaching the bus. */
    return *(const volatile uint32_t *)(uintptr_t)address;
}
