/* Context switch for the YanOS cooperative runtime.
 *
 * void yan_os_task_switch(YanOsTaskContext *from, YanOsTaskContext *to);
 *
 * Saves the running context into `from`, loads `to` and returns into it. Only
 * the callee-saved set of the RISC-V calling convention is touched (ra, sp,
 * s0-s11): this function is reached through an ordinary C call, so every
 * caller-saved register is already saved or dead at the call site, and saving
 * them here would do the same work twice. The offsets come from os/task.h,
 * where task.c asserts them against its struct.
 *
 * The first switch into a task was prepared by yan_os_task_spawn: `ra` holds
 * the address of the task trampoline and `sp` the top of the task's own static
 * stack, so the `ret` below enters the task on its own stack.
 *
 * The two pointer arguments are caller-saved by the calling convention, so
 * they need no preservation across this call; they are used before the restore
 * half and not after it.
 */
#include "task.h"

  .text
  .align 2
  .globl yan_os_task_switch
  .type yan_os_task_switch, @function
yan_os_task_switch:
  sw ra, YAN_OS_TASK_CTX_RA(a0)
  sw sp, YAN_OS_TASK_CTX_SP(a0)
  sw s0, YAN_OS_TASK_CTX_S0(a0)
  sw s1, YAN_OS_TASK_CTX_S1(a0)
  sw s2, YAN_OS_TASK_CTX_S2(a0)
  sw s3, YAN_OS_TASK_CTX_S3(a0)
  sw s4, YAN_OS_TASK_CTX_S4(a0)
  sw s5, YAN_OS_TASK_CTX_S5(a0)
  sw s6, YAN_OS_TASK_CTX_S6(a0)
  sw s7, YAN_OS_TASK_CTX_S7(a0)
  sw s8, YAN_OS_TASK_CTX_S8(a0)
  sw s9, YAN_OS_TASK_CTX_S9(a0)
  sw s10, YAN_OS_TASK_CTX_S10(a0)
  sw s11, YAN_OS_TASK_CTX_S11(a0)

  lw ra, YAN_OS_TASK_CTX_RA(a1)
  lw sp, YAN_OS_TASK_CTX_SP(a1)
  lw s0, YAN_OS_TASK_CTX_S0(a1)
  lw s1, YAN_OS_TASK_CTX_S1(a1)
  lw s2, YAN_OS_TASK_CTX_S2(a1)
  lw s3, YAN_OS_TASK_CTX_S3(a1)
  lw s4, YAN_OS_TASK_CTX_S4(a1)
  lw s5, YAN_OS_TASK_CTX_S5(a1)
  lw s6, YAN_OS_TASK_CTX_S6(a1)
  lw s7, YAN_OS_TASK_CTX_S7(a1)
  lw s8, YAN_OS_TASK_CTX_S8(a1)
  lw s9, YAN_OS_TASK_CTX_S9(a1)
  lw s10, YAN_OS_TASK_CTX_S10(a1)
  lw s11, YAN_OS_TASK_CTX_S11(a1)
  ret
  .size yan_os_task_switch, .-yan_os_task_switch
