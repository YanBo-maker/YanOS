/* Guest entry point shared by every YanOS validation program.
 *
 * The `tohost` convention matches the official RISC-V test suites: a nonzero
 * word ends the run. The symbol is looked up through the image symbol table by
 * the validation tools, so its address is a link-time decision, not a
 * hard-coded constant. */
  .section .text.start, "ax"
  .globl _start
_start:
  la sp, __guest_stack_top
  la t0, tohost
  sw zero, 0(t0)
  call main
  la t0, tohost
  li t1, 0xdead
  sw t1, 0(t0)
.Lhang:
  j .Lhang

  .section .bss
  .align 4
__guest_stack:
  .space 16384
  .globl __guest_stack_top
__guest_stack_top:

  .section .tohost, "aw"
  .align 2
  .globl tohost
tohost:
  .word 0
