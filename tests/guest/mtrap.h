#ifndef MTRAP_H
#define MTRAP_H

#include <stdint.h>

/* Minimal M-mode Guest trap ABI.
 *
 * Contract, in one place:
 *
 *   entry      `_start` in mtrap_entry.S, placed first by link.ld at the image
 *              base. It sets sp, clears `tohost`, calls yan_guest_boot() and
 *              then calls main(). main() is not expected to return.
 *   stack      `__yan_guest_stack_top`, 16 KiB in .bss.
 *   vector     `__yan_guest_trap_vector`, 4-byte aligned, installed in mtvec in
 *              direct mode by yan_guest_boot(). The CPU clears mstatus.MIE on
 *              entry, so the vector body cannot nest.
 *   frame      a fixed 20-word frame on the interrupted stack; see
 *              mtrap_frame.h. The interrupted sp is saved in it, so the
 *              handler may use the stack normally.
 *   dispatch   the vector calls yan_guest_trap_dispatch(cause, epc, tval). The
 *              returned value is written to mepc and the vector executes
 *              `mret`, so a handler decides where execution resumes: `epc` to
 *              re-run the interrupted instruction (interrupts), `epc + 4` to
 *              step over a synchronous exception.
 *   cause      a synchronous exception uses its code from the privileged spec;
 *              an interrupt has YAN_GUEST_CAUSE_INTERRUPT set and the same code
 *              in the low bits (MSIP 3, MTIP 7, MEIP 11).
 *
 * The ABI deliberately touches only the CSRs the Yan platform implements:
 * mstatus, mie, mip, mtvec, mepc, mcause, mtval and mscratch. There is no
 * delegation, vector mode, nesting or U/S mode here.
 */

/* mstatus.MIE, the machine-level interrupt enable. */
#define YAN_GUEST_MSTATUS_MIE UINT32_C(0x8)

/* mip / mie bit positions, which equal the interrupt cause codes. They live
 * here rather than with the device map because they describe CPU CSR state. */
#define YAN_GUEST_MIE_MSIP UINT32_C(0x00000008)
#define YAN_GUEST_MIE_MTIP UINT32_C(0x00000080)
#define YAN_GUEST_MIE_MEIP UINT32_C(0x00000800)
#define YAN_GUEST_MIE_MASK \
    (YAN_GUEST_MIE_MSIP | YAN_GUEST_MIE_MTIP | YAN_GUEST_MIE_MEIP)

/* Implemented CSR numbers. Kept as constants because the trap ABI documents
 * them by number as well as by name. */
#define YAN_GUEST_CSR_MSTATUS UINT32_C(0x300)
#define YAN_GUEST_CSR_MIE UINT32_C(0x304)
#define YAN_GUEST_CSR_MTVEC UINT32_C(0x305)
#define YAN_GUEST_CSR_MSCRATCH UINT32_C(0x340)
#define YAN_GUEST_CSR_MEPC UINT32_C(0x341)
#define YAN_GUEST_CSR_MCAUSE UINT32_C(0x342)
#define YAN_GUEST_CSR_MTVAL UINT32_C(0x343)

/* Synchronous exception codes (privileged spec, "Machine Cause" register). */
#define YAN_GUEST_CAUSE_INSTR_ADDR_MISALIGNED UINT32_C(0)
#define YAN_GUEST_CAUSE_INSTR_ACCESS_FAULT UINT32_C(1)
#define YAN_GUEST_CAUSE_ILLEGAL_INSTRUCTION UINT32_C(2)
#define YAN_GUEST_CAUSE_BREAKPOINT UINT32_C(3)
#define YAN_GUEST_CAUSE_LOAD_MISALIGNED UINT32_C(4)
#define YAN_GUEST_CAUSE_LOAD_ACCESS_FAULT UINT32_C(5)
#define YAN_GUEST_CAUSE_STORE_MISALIGNED UINT32_C(6)
#define YAN_GUEST_CAUSE_STORE_ACCESS_FAULT UINT32_C(7)
#define YAN_GUEST_CAUSE_ECALL_M UINT32_C(11)

/* Interrupt marker and the three machine-level interrupt codes. */
#define YAN_GUEST_CAUSE_INTERRUPT UINT32_C(0x80000000)
#define YAN_GUEST_CAUSE_MSIP UINT32_C(3)
#define YAN_GUEST_CAUSE_MTIP UINT32_C(7)
#define YAN_GUEST_CAUSE_MEIP UINT32_C(11)

/* Reported through `tohost` when the default handler is reached. */
#define YAN_GUEST_TRAP_PANIC UINT32_C(0xbad0)

/* Returns the pc at which execution resumes; the vector writes it to mepc. */
typedef uint32_t (*yan_guest_trap_fn)(uint32_t cause, uint32_t epc,
                                      uint32_t tval);

/* Defined by mtrap_entry.S. */
extern char __yan_guest_trap_vector[];

uint32_t yan_guest_trap_vector(void);

/* Installs the direct vector, clears mie and starts with mstatus.MIE = 0. */
void yan_guest_boot(void);
/* Replaces the handler the vector dispatches to. */
void yan_guest_trap_install(yan_guest_trap_fn handler);
yan_guest_trap_fn yan_guest_trap_handler(void);
/* Called by the vector; forwards to the installed handler. */
uint32_t yan_guest_trap_dispatch(uint32_t cause, uint32_t epc, uint32_t tval);
/* Default handler: ends the run with YAN_GUEST_TRAP_PANIC | cause. */
uint32_t yan_guest_trap_panic(uint32_t cause, uint32_t epc, uint32_t tval);

/* Machine-mode CSR state a Guest may observe. */
uint32_t yan_guest_mstatus(void);
uint32_t yan_guest_mie(void);
uint32_t yan_guest_mip(void);
uint32_t yan_guest_mtvec(void);

/* Enables the given mie bits and then mstatus.MIE; disabling clears MIE only,
 * so the enabled lines stay configured for the next enable. */
void yan_guest_enable_interrupts(uint32_t mask);
void yan_guest_disable_interrupts(void);

/* Instruction helpers used by the self-check to raise each trap it expects. */
void yan_guest_ecall(void);
void yan_guest_ebreak(void);
/* Confirms the vector returned a live caller-saved register intact: the frame
 * only covers that set, so a wrong slot is an ABI violation, not a detail. */
uint32_t yan_guest_probe_register_preservation(void);
uint32_t yan_guest_probe_unsupported_csr(void);
uint32_t yan_guest_load32(uint32_t address);

#endif
