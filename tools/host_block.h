#ifndef YAN_HOST_BLOCK_H
#define YAN_HOST_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yan/ram.h"
#include "yan/status.h"
#include "yan/transport.h"

/* Host side of the block request protocol in
 * docs/specs/0018-block-protocol.md: a memory-backed block device plus the
 * codec for the request and response frames.
 *
 * This is a host program, not a device. Nothing under src/ knows it exists: the
 * channel moves no bytes and holds no RAM pointer (docs/specs/0014), so the
 * service reaches the rings through the YanRam it is handed. The Guest cannot
 * tell whether the other end is a file, memory or a real disk, which is the
 * point of putting the protocol on the channel instead of on device MMIO. */

/* The protocol counts blocks, not bytes, and the block size is the one YanFS
 * will use, so no repacking is needed between the two layers. */
#define YAN_HOST_BLOCK_BLOCK_SIZE UINT32_C(4096)

typedef struct {
    /* Backing store: capacity_blocks * YAN_HOST_BLOCK_BLOCK_SIZE bytes. It stays
     * owned by the caller so a test can plant contents and inspect them after a
     * run. */
    uint8_t *storage;
    uint64_t capacity_blocks;
    /* Requests answered since init. The throughput benchmark reads it and the
     * tests assert on it, which is why it lives in the struct and not in a
     * local of the pump. */
    uint64_t served;
    /* Fault injection: while set, the next request that touches storage is
     * answered with status 2 and the flag clears. 0018 requires the failure path
     * to be observable, and a memory device never fails on its own, so the
     * failure has to be reachable on demand. */
    bool fail_next;
} YanHostBlock;

/* storage must hold capacity_blocks * YAN_HOST_BLOCK_BLOCK_SIZE bytes; the
 * caller keeps ownership and the bytes are used as they are. A capacity of zero
 * is valid here and means every request is answered as out of range, so the
 * caller does not have to special-case it; yan_run rejects it at the command
 * line instead. */
YanStatus yan_host_block_init(YanHostBlock *block, uint8_t *storage,
                              uint64_t capacity_blocks);

/* Answer every complete request the guest has published, in arrival order, and
 * return how many were answered. A frame that has not fully arrived is left in
 * the ring untouched: 0018 requires the whole header before a byte of it is
 * interpreted, and requires a frame that fails validation to be consumed by its
 * full length so the stream does not lose its alignment.
 *
 * The service never blocks and never waits for a request: it answers what is
 * there and returns. ram_base is the address ram was initialised with, and is
 * needed because ring_base is an address while the RAM accessors take offsets.
 */
uint32_t yan_host_block_service(YanHostBlock *block, YanTransport *transport,
                                YanRam *ram, uint32_t ram_base);

#endif
