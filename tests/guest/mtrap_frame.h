#ifndef MTRAP_FRAME_H
#define MTRAP_FRAME_H

/* Trap-vector frame layout, in 32-bit words from sp on entry to
 * __yan_guest_trap_vector.
 *
 * The vector subtracts YAN_GUEST_TRAP_FRAME_BYTES from sp first, so it never
 * has to park the interrupted sp in a second register: that value is always
 * `sp + YAN_GUEST_TRAP_FRAME_BYTES` while the frame is live, and it is stored
 * in the last slot so the epilogue can restore it.
 *
 * Only caller-saved registers are stored. The C dispatcher follows the RV32
 * calling convention, so it preserves the callee-saved registers (sp, s0-s1,
 * s2-s11) by construction; the set below is exactly the set it may clobber.
 *
 * The frame is 80 bytes so that sp stays 16-byte aligned, which is the RV32
 * psABI requirement for the `call` the vector makes.
 */
#define YAN_GUEST_TRAP_RA 0
#define YAN_GUEST_TRAP_T0 1
#define YAN_GUEST_TRAP_T1 2
#define YAN_GUEST_TRAP_T2 3
#define YAN_GUEST_TRAP_A0 4
#define YAN_GUEST_TRAP_A1 5
#define YAN_GUEST_TRAP_A2 6
#define YAN_GUEST_TRAP_A3 7
#define YAN_GUEST_TRAP_A4 8
#define YAN_GUEST_TRAP_A5 9
#define YAN_GUEST_TRAP_A6 10
#define YAN_GUEST_TRAP_A7 11
#define YAN_GUEST_TRAP_T3 12
#define YAN_GUEST_TRAP_T4 13
#define YAN_GUEST_TRAP_T5 14
#define YAN_GUEST_TRAP_T6 15
#define YAN_GUEST_TRAP_SP 16

#define YAN_GUEST_TRAP_WORDS 20
#define YAN_GUEST_TRAP_FRAME_BYTES (YAN_GUEST_TRAP_WORDS * 4)

#endif
