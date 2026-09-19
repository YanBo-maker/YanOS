/* A Guest that deliberately performs a misaligned word load.
 *
 * YanCPU traps on it by design (mcause = 4); the NEMU riscv32 reference has no
 * alignment check at all. The case therefore exercises the boundary between
 * "the DUT diverged" and "the DUT did something the reference cannot mirror":
 * the tester must report it as out of scope, not as an architectural mismatch.
 * `run_scope.sh` asserts exactly that. */
  .section .text.start, "ax"
  .globl _start
_start:
  la t0, tohost
  sw zero, 0(t0)
  li t1, 0x80002001
  lw t2, 0(t1)
  la t0, tohost
  li t3, 1
  sw t3, 0(t0)
.Lspin:
  j .Lspin

  .section .bss
  .align 4
padding:
  .space 16

  .section .tohost, "aw"
  .align 2
  .globl tohost
tohost:
  .word 0
