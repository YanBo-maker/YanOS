# rvmodel_macros.h
# YanOS DUT-specific macro definitions for the RISC-V ACT4 framework.
#
# YanOS implements RV32IM, Zicsr for the M-mode CSR set (mstatus, mtvec,
# mscratch, mepc, mcause, mtval, mie, mip) and Zifencei. The platform has a
# single-hart CLINT and a single-context M-mode PLIC. It has no S/U mode, no
# interrupt delegation, no interrupt nesting and no PMP.
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
# The registers behind those macros now exist, but this ACT4 path still does not
# adapt the framework's interrupt flow (no RVMODEL_MSIP_ADDRESS /
# RVMODEL_MTIME_ADDRESS plumbing, and the framework's standard M-mode startup
# still writes delegation CSRs YanOS does not have), so the corpus check keeps
# refusing every interrupt case instead of reporting a result it cannot stand
# behind.
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

##### MACHINE TIMER AND SOFTWARE INTERRUPT #####

# YanOS has a single-hart CLINT at the standard base. The offsets are the ones
# the Host header include/yan/interrupt.h implements, and they are also the ones
# sail_macros.h derives from SAIL_CLINT_BASE_ADDRESS, so the DUT description and
# the framework's effective addresses agree by construction.
#
# The framework #undefs these and substitutes its SAIL_* values, so declaring
# them here cannot change generated code. What it does change is that a
# mismatch becomes detectable: tests/official/run_act4.sh assembles a probe
# that fails if the effective addresses differ from the ones declared here.
#define YANOS_CLINT_BASE_ADDRESS 0x02000000
#define RVMODEL_MSIP_ADDRESS (YANOS_CLINT_BASE_ADDRESS + 0x0)
#define RVMODEL_MTIMECMP_ADDRESS (YANOS_CLINT_BASE_ADDRESS + 0x4000)
#define RVMODEL_MTIME_ADDRESS (YANOS_CLINT_BASE_ADDRESS + 0xbff8)

##### UNSUPPORTED CAPABILITIES #####

# External interrupts have no Guest-visible source: YanOS's PLIC has no raise
# register, and only the platform can drive a source line through
# yan_plic_set_level(). A device reports a line level and the PLIC decides
# whether that becomes MEIP. The framework's own RVMODEL_SET_MEXT_INT writes to
# a Sail test-interrupt-generator device (SAIL_SIG_ADDRESS) that this platform
# does not have, and adding a device for it is out of scope. These macros must
# exist for the framework's checks; see the note at the top of this file about
# which guard is actually in effect.
#define RVMODEL_SET_MEXT_INT(_R1, _R2) .error "YanOS has no external interrupt source register"
#define RVMODEL_CLR_MEXT_INT(_R1, _R2) .error "YanOS has no external interrupt source register"

# Software interrupts go through the CLINT msip register, which the address
# macro above already describes, so the platform-specific pair stays an error.
#define RVMODEL_SET_MSW_INT(_R1, _R2)  .error "use RVMODEL_MSIP_ADDRESS for software interrupts"
#define RVMODEL_CLR_MSW_INT(_R1, _R2)  .error "use RVMODEL_MSIP_ADDRESS for software interrupts"

# Required by the framework's checks. No timer test reaches these values today:
# every helper that reads them is gated on STANDARD_SM_SUPPORTED below.
#define RVMODEL_INTERRUPT_LATENCY 1
#define RVMODEL_TIMER_INT_SOON_DELAY 1
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 1

# STANDARD_SM_SUPPORTED stays unset. It gates the framework's trap handler
# instantiation and its whole M-mode interrupt helper bank, and it assumes CSR
# state YanOS deliberately does not implement: medeleg/mideleg for delegation,
# PMP registers, and S-mode. mie, mip and the CLINT now exist, but they are not
# what this switch needs. The consequence is explicit and verified in
# tests/official/run_act4.sh: the seven branch and jump tests that expect a trap
# handler are reported as SKIP with that reason, never as passes.

#endif // _RVMODEL_MACROS_H
