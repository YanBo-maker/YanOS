#ifndef GUEST_DEVICES_H
#define GUEST_DEVICES_H

#include <stdint.h>

/* Guest-visible view of the platform devices.
 *
 * A freestanding Guest build cannot include the Host's include/yan/interrupt.h:
 * that header pulls in Host types and only makes sense in the emulator's own
 * translation units. The map is therefore restated here, and
 * tests/test_boot.c compares the two descriptions constant by constant so the
 * restatement cannot silently drift.
 *
 * Everything below is a direct 32-bit word access to a memory-mapped register.
 * The devices answer word accesses only, and YanCPU traps a Guest that uses any
 * other width, so the accessors must not be rewritten to byte or halfword
 * operations.
 */

#define YAN_GUEST_CLINT_BASE UINT32_C(0x02000000)
#define YAN_GUEST_CLINT_SIZE UINT32_C(0x00010000)
#define YAN_GUEST_CLINT_MSIP UINT32_C(0x0000)
#define YAN_GUEST_CLINT_MTIMECMP UINT32_C(0x4000)
#define YAN_GUEST_CLINT_MTIME UINT32_C(0xbff8)

#define YAN_GUEST_PLIC_BASE UINT32_C(0x0c000000)
#define YAN_GUEST_PLIC_SIZE UINT32_C(0x00400000)
#define YAN_GUEST_PLIC_PRIORITY UINT32_C(0x0000)
#define YAN_GUEST_PLIC_PENDING UINT32_C(0x1000)
#define YAN_GUEST_PLIC_ENABLE_M UINT32_C(0x2000)
#define YAN_GUEST_PLIC_THRESHOLD_M UINT32_C(0x200000)
#define YAN_GUEST_PLIC_CLAIM_M UINT32_C(0x200004)
#define YAN_GUEST_PLIC_SOURCE_MAX 31

static inline uint32_t yan_guest_mmio_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static inline void yan_guest_mmio_write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

/* CLINT. mtimecmp and mtime are 64-bit split into two 32-bit registers; there
 * is one hart and no asynchronous writer, so a plain low/high pair is safe and
 * needs no latching handshake. */
static inline void yan_guest_clint_set_msip(uint32_t value)
{
    yan_guest_mmio_write32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MSIP,
                           value & UINT32_C(1));
}

static inline uint32_t yan_guest_clint_msip(void)
{
    return yan_guest_mmio_read32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MSIP) &
           UINT32_C(1);
}

static inline void yan_guest_clint_clear_msip(void)
{
    yan_guest_clint_set_msip(0);
}

static inline void yan_guest_clint_set_mtimecmp(uint64_t deadline)
{
    yan_guest_mmio_write32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIMECMP,
                           (uint32_t)deadline);
    yan_guest_mmio_write32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIMECMP + 4,
                           (uint32_t)(deadline >> 32));
}

static inline uint64_t yan_guest_clint_mtimecmp(void)
{
    const uint32_t low =
        yan_guest_mmio_read32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIMECMP);
    const uint32_t high = yan_guest_mmio_read32(
        YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIMECMP + 4);
    return ((uint64_t)high << 32) | low;
}

/* Pushing the deadline out of reach is how a Guest acknowledges MTIP. */
static inline void yan_guest_clint_disarm_timer(void)
{
    yan_guest_clint_set_mtimecmp(UINT64_MAX);
}

static inline void yan_guest_clint_set_mtime(uint64_t instant)
{
    yan_guest_mmio_write32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIME,
                           (uint32_t)instant);
    yan_guest_mmio_write32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIME + 4,
                           (uint32_t)(instant >> 32));
}

static inline uint64_t yan_guest_clint_mtime(void)
{
    const uint32_t low =
        yan_guest_mmio_read32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIME);
    const uint32_t high =
        yan_guest_mmio_read32(YAN_GUEST_CLINT_BASE + YAN_GUEST_CLINT_MTIME + 4);
    return ((uint64_t)high << 32) | low;
}

/* PLIC, single M-mode context. The Guest cannot raise an external source: this
 * platform exposes no raise register, so only the Host can assert that line. */
static inline void yan_guest_plic_set_priority(uint32_t source, uint32_t value)
{
    yan_guest_mmio_write32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_PRIORITY +
                               4 * source,
                           value);
}

static inline uint32_t yan_guest_plic_priority(uint32_t source)
{
    return yan_guest_mmio_read32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_PRIORITY +
                                 4 * source);
}

static inline void yan_guest_plic_set_enable(uint32_t mask)
{
    yan_guest_mmio_write32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_ENABLE_M, mask);
}

static inline uint32_t yan_guest_plic_enable(void)
{
    return yan_guest_mmio_read32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_ENABLE_M);
}

static inline void yan_guest_plic_set_threshold(uint32_t value)
{
    yan_guest_mmio_write32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_THRESHOLD_M,
                           value);
}

static inline uint32_t yan_guest_plic_threshold(void)
{
    return yan_guest_mmio_read32(YAN_GUEST_PLIC_BASE +
                                 YAN_GUEST_PLIC_THRESHOLD_M);
}

static inline uint32_t yan_guest_plic_pending(void)
{
    return yan_guest_mmio_read32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_PENDING);
}

/* Reading the claim register is a handshake, not a plain read. */
static inline uint32_t yan_guest_plic_claim(void)
{
    return yan_guest_mmio_read32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_CLAIM_M);
}

static inline void yan_guest_plic_complete(uint32_t source)
{
    yan_guest_mmio_write32(YAN_GUEST_PLIC_BASE + YAN_GUEST_PLIC_CLAIM_M,
                           source);
}

#endif
