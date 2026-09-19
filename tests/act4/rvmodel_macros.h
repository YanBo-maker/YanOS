# rvmodel_macros.h
# YanOS DUT-specific macro definitions for the RISC-V ACT4 framework.
#
# YanOS implements RV32IM, Zicsr for a six-register M-mode set (mstatus, mtvec,
# mscratch, mepc, mcause, mtval) and Zifencei. It has no interrupt controller,
# no timer, no S/U mode and no PMP.
#
# Note on the interrupt macros below. They are defined to a hard assembler
# failure, but that guard does NOT take effect in this framework: the
# environment includes `sail_macros.h` after this header and redefines these
# macros into real CLINT/PLIC implementations, so the framework always wins.
# The guard that actually holds is in `tests/official/run_act4.sh`: it refuses
# any test whose source invokes an interrupt macro. The bodies here are kept
# only so a hand-built test that includes this header without the framework
# still fails loudly instead of writing to an address that does not exist.
#
# SPDX-License-Identifier: BSD-3-Clause

#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

##### DATA AND TERMINATION #####

# tohost is the halt word: yan_run finds it through the ELF symbol table and a
# nonzero value ends the run. fromhost is kept because the framework's layout
# expects it.
#define RVMODEL_DATA_SECTION \
        .pushsection .tohost,"aw",@progbits;             \
        .balign 8; .global tohost; tohost: .dword 0;     \
        .balign 8; .global fromhost; fromhost: .dword 0; \
        .popsection

#define RVMODEL_HALT_PASS  \
  li x1, 1                 ;\
  la t0, tohost            ;\
  rvmodel_halt_pass:       ;\
    sw x1, 0(t0)           ;\
    j rvmodel_halt_pass    ;

#define RVMODEL_HALT_FAIL  \
  li x1, 3                 ;\
  la t0, tohost            ;\
  rvmodel_halt_fail:       ;\
    sw x1, 0(t0)           ;\
    j rvmodel_halt_fail    ;

##### IO #####

# The CPU has no console attached, so failure text is not printed. Pass or fail
# is decided by the signature comparison against the reference model.
#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR)

##### UNSUPPORTED CAPABILITIES #####

# YanOS has no PLIC and no CLINT. These macros must exist for the framework's
# checks. See the note at the top of this file: the framework replaces them, so
# the effective guard is the corpus check in tests/official/run_act4.sh.
#define RVMODEL_SET_MEXT_INT(_R1, _R2) .error "YanOS has no external interrupt controller"
#define RVMODEL_CLR_MEXT_INT(_R1, _R2) .error "YanOS has no external interrupt controller"
#define RVMODEL_SET_MSW_INT(_R1, _R2)  .error "YanOS has no software interrupt register"
#define RVMODEL_CLR_MSW_INT(_R1, _R2)  .error "YanOS has no software interrupt register"

# Required by the framework's checks even though no timer test can run here.
#define RVMODEL_INTERRUPT_LATENCY 1
#define RVMODEL_TIMER_INT_SOON_DELAY 1
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 1

# The standard M-mode CSR bank the framework assumes (mie, mip, delegation,
# PMP) does not exist on YanOS, so STANDARD_SM_SUPPORTED is deliberately left
# unset and that initialisation block is skipped.

#endif // _RVMODEL_MACROS_H
