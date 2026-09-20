/* Minimal M-mode Guest entry point and trap vector.
 *
 * This file replaces start.S for Guest programs that need a trap handler: it
 * adds the mtvec installation path and a direct vector that saves the
 * caller-saved registers, calls the C dispatcher, writes the dispatcher's
 * return value into mepc and returns with mret.
 *
 * start.S deliberately stays untouched. It is the entry point of the Guest
 * corpus that the differential tester runs against a reference model with no
 * CSR state, and adding `csrw` there would push those images outside that
 * model's scope.
 *
 * Frame layout: mtrap_frame.h. Contract: mtrap.h.
 */
#include "mtrap_frame.h"

  .section .text.start, "ax"
  .globl _start
_start:
  la sp, __yan_guest_stack_top
  la t0, tohost
  sw zero, 0(t0)
  call yan_guest_boot
  call main
  /* main() is not expected to return; if it does, the run did not reach its
   * final check and must not be reported as a pass. */
  la t0, tohost
  li t1, 0xdead
  sw t1, 0(t0)
.Lhang:
  j .Lhang

/* Direct trap vector, installed in mtvec by yan_guest_boot(). The CPU enters
 * with mstatus.MIE already cleared, so this body never nests.
 *
 * The frame is pushed on the interrupted stack. The interrupted sp is the
 * frame's upper edge, which is where the epilogue recovers it from. */
  .align 4
  .globl __yan_guest_trap_vector
__yan_guest_trap_vector:
  addi sp, sp, -YAN_GUEST_TRAP_FRAME_BYTES

  sw ra, (YAN_GUEST_TRAP_RA * 4)(sp)
  sw t0, (YAN_GUEST_TRAP_T0 * 4)(sp)
  sw t1, (YAN_GUEST_TRAP_T1 * 4)(sp)
  sw t2, (YAN_GUEST_TRAP_T2 * 4)(sp)
  sw a0, (YAN_GUEST_TRAP_A0 * 4)(sp)
  sw a1, (YAN_GUEST_TRAP_A1 * 4)(sp)
  sw a2, (YAN_GUEST_TRAP_A2 * 4)(sp)
  sw a3, (YAN_GUEST_TRAP_A3 * 4)(sp)
  sw a4, (YAN_GUEST_TRAP_A4 * 4)(sp)
  sw a5, (YAN_GUEST_TRAP_A5 * 4)(sp)
  sw a6, (YAN_GUEST_TRAP_A6 * 4)(sp)
  sw a7, (YAN_GUEST_TRAP_A7 * 4)(sp)
  sw t3, (YAN_GUEST_TRAP_T3 * 4)(sp)
  sw t4, (YAN_GUEST_TRAP_T4 * 4)(sp)
  sw t5, (YAN_GUEST_TRAP_T5 * 4)(sp)
  sw t6, (YAN_GUEST_TRAP_T6 * 4)(sp)

  /* t0's saved value is in the frame, so it is free to carry the interrupted
   * sp into the last slot. */
  addi t0, sp, YAN_GUEST_TRAP_FRAME_BYTES
  sw t0, (YAN_GUEST_TRAP_SP * 4)(sp)

  csrr a0, mcause
  csrr a1, mepc
  csrr a2, mtval
  call yan_guest_trap_dispatch

  /* The dispatcher's return value is the pc to resume at. */
  csrw mepc, a0

  lw ra, (YAN_GUEST_TRAP_RA * 4)(sp)
  lw t0, (YAN_GUEST_TRAP_T0 * 4)(sp)
  lw t1, (YAN_GUEST_TRAP_T1 * 4)(sp)
  lw t2, (YAN_GUEST_TRAP_T2 * 4)(sp)
  lw a0, (YAN_GUEST_TRAP_A0 * 4)(sp)
  lw a1, (YAN_GUEST_TRAP_A1 * 4)(sp)
  lw a2, (YAN_GUEST_TRAP_A2 * 4)(sp)
  lw a3, (YAN_GUEST_TRAP_A3 * 4)(sp)
  lw a4, (YAN_GUEST_TRAP_A4 * 4)(sp)
  lw a5, (YAN_GUEST_TRAP_A5 * 4)(sp)
  lw a6, (YAN_GUEST_TRAP_A6 * 4)(sp)
  lw a7, (YAN_GUEST_TRAP_A7 * 4)(sp)
  lw t3, (YAN_GUEST_TRAP_T3 * 4)(sp)
  lw t4, (YAN_GUEST_TRAP_T4 * 4)(sp)
  lw t5, (YAN_GUEST_TRAP_T5 * 4)(sp)
  lw t6, (YAN_GUEST_TRAP_T6 * 4)(sp)
  /* Restoring sp last: the address is computed from the frame pointer before
   * the write, so this also drops the frame. */
  lw sp, (YAN_GUEST_TRAP_SP * 4)(sp)

  mret

  .section .bss
  .align 4
__yan_guest_stack:
  .space 16384
  .globl __yan_guest_stack_top
__yan_guest_stack_top:

  .section .tohost, "aw"
  .align 2
  .globl tohost
tohost:
  .word 0
