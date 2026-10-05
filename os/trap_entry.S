/* YanOS runtime entry point and trap vector.
 *
 * This file is the self-contained counterpart of tests/guest/mtrap_entry.S for
 * images that link os/task.c: os/ must not reference tests/ (0017), so the
 * entry point, the vector, the boot stack and tohost all live here.
 *
 *   _start       sets sp, clears tohost, installs the Direct vector, calls
 *                main, and treats a returning main as an unfinished run.
 *   the vector   pushes a fixed 20-word frame on the interrupted stack, saves
 *                the caller-saved registers and the interrupted sp, calls the
 *                C handler in task.c, writes the handler's answer into mepc and
 *                returns with mret. That shape follows 0013, which is already
 *                independently verified: only caller-saved registers are
 *                stored, because the C handler preserves sp and s0-s11 by
 *                construction, and the frame is 80 bytes so sp stays 16-byte
 *                aligned for the call.
 *
 * The frame lives on the interrupted task's stack, so an interrupt is entered
 * and left without touching any task's saved context; the handler marks
 * waiters, it does not switch.
 *
 * The sixteen slots are the calling convention's caller-saved set rather than a
 * measured minimum: at -O2 the C handler happens not to dirty all of them, so a
 * probe that only watches the restore side observes fewer registers than the
 * frame holds. The save side is observable slot by slot; the restore side is
 * bounded by what the handler actually clobbers.
 */

/* Frame layout, in bytes from sp on entry to the vector. The interrupted sp is
 * the frame's upper edge and is stored in the last slot, which is where the
 * epilogue recovers it from. */
#define YAN_OS_TRAP_RA 0
#define YAN_OS_TRAP_T0 4
#define YAN_OS_TRAP_T1 8
#define YAN_OS_TRAP_T2 12
#define YAN_OS_TRAP_A0 16
#define YAN_OS_TRAP_A1 20
#define YAN_OS_TRAP_A2 24
#define YAN_OS_TRAP_A3 28
#define YAN_OS_TRAP_A4 32
#define YAN_OS_TRAP_A5 36
#define YAN_OS_TRAP_A6 40
#define YAN_OS_TRAP_A7 44
#define YAN_OS_TRAP_T3 48
#define YAN_OS_TRAP_T4 52
#define YAN_OS_TRAP_T5 56
#define YAN_OS_TRAP_T6 60
#define YAN_OS_TRAP_SP 64
#define YAN_OS_TRAP_FRAME_BYTES 80

  .section .text.start, "ax"
  .globl _start
_start:
  la sp, __yan_os_boot_stack_top
  la t0, tohost
  sw zero, 0(t0)
  la t0, __yan_os_trap_vector
  csrw mtvec, t0
  call main
  /* main() is not expected to return: the scheduler never leaves its loop, so
   * getting here means the run did not reach its final check and must not be
   * reported as a pass. */
  la t0, tohost
  li t1, 0xdead
  sw t1, 0(t0)
.Lhang:
  j .Lhang

/* Direct trap vector. The CPU enters with mstatus.MIE already cleared and
 * mstatus.MPIE holding the interrupted value, so this body never nests and
 * mret restores the interrupt state exactly as it was. */
  .align 4
  .globl __yan_os_trap_vector
__yan_os_trap_vector:
  addi sp, sp, -YAN_OS_TRAP_FRAME_BYTES

  sw ra, YAN_OS_TRAP_RA(sp)
  sw t0, YAN_OS_TRAP_T0(sp)
  sw t1, YAN_OS_TRAP_T1(sp)
  sw t2, YAN_OS_TRAP_T2(sp)
  sw a0, YAN_OS_TRAP_A0(sp)
  sw a1, YAN_OS_TRAP_A1(sp)
  sw a2, YAN_OS_TRAP_A2(sp)
  sw a3, YAN_OS_TRAP_A3(sp)
  sw a4, YAN_OS_TRAP_A4(sp)
  sw a5, YAN_OS_TRAP_A5(sp)
  sw a6, YAN_OS_TRAP_A6(sp)
  sw a7, YAN_OS_TRAP_A7(sp)
  sw t3, YAN_OS_TRAP_T3(sp)
  sw t4, YAN_OS_TRAP_T4(sp)
  sw t5, YAN_OS_TRAP_T5(sp)
  sw t6, YAN_OS_TRAP_T6(sp)

  /* t0's saved value is in the frame, so it is free to carry the interrupted
   * sp into the last slot. */
  addi t0, sp, YAN_OS_TRAP_FRAME_BYTES
  sw t0, YAN_OS_TRAP_SP(sp)

  csrr a0, mcause
  csrr a1, mepc
  csrr a2, mtval
  call yan_os_trap_handler

  /* The handler's return value is the pc to resume at. For an interrupt that
   * is the interrupted instruction itself. */
  csrw mepc, a0

  lw ra, YAN_OS_TRAP_RA(sp)
  lw t0, YAN_OS_TRAP_T0(sp)
  lw t1, YAN_OS_TRAP_T1(sp)
  lw t2, YAN_OS_TRAP_T2(sp)
  lw a0, YAN_OS_TRAP_A0(sp)
  lw a1, YAN_OS_TRAP_A1(sp)
  lw a2, YAN_OS_TRAP_A2(sp)
  lw a3, YAN_OS_TRAP_A3(sp)
  lw a4, YAN_OS_TRAP_A4(sp)
  lw a5, YAN_OS_TRAP_A5(sp)
  lw a6, YAN_OS_TRAP_A6(sp)
  lw a7, YAN_OS_TRAP_A7(sp)
  lw t3, YAN_OS_TRAP_T3(sp)
  lw t4, YAN_OS_TRAP_T4(sp)
  lw t5, YAN_OS_TRAP_T5(sp)
  lw t6, YAN_OS_TRAP_T6(sp)
  /* Restoring sp last drops the frame: the address is read from the frame
   * before the load overwrites the register. */
  lw sp, YAN_OS_TRAP_SP(sp)

  mret

  .section .bss
  .align 4
__yan_os_boot_stack:
  .space 16384
  .globl __yan_os_boot_stack_top
__yan_os_boot_stack_top:

  .section .tohost, "aw"
  .align 2
  .globl tohost
tohost:
  .word 0
