#ifndef YAN_OS_YANFS_BLOCK_H
#define YAN_OS_YANFS_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "yanfs.h"

/* Guest adapter from the block protocol (docs/specs/0018-block-protocol.md, on
 * the channel of docs/specs/0014-host-transport-channel.md) to the YanFS
 * YanFsBlockIo callbacks (docs/specs/0021-yanfs.md, "Guest 适配公开接口").
 *
 * The adapter owns one request/response pairing at a time: it submits exactly
 * one request with yan_os_block_submit(), waits with the cooperative runtime
 * predicate until a complete response frame is published, consumes it with
 * yan_os_block_take(), then checks it against the recorded request. It never
 * spins on the guest-to-host ring: there is no "space became available"
 * interrupt in this hardware, so a submit that cannot fit is a protocol
 * failure, not something to wait out.
 *
 * The whole transport channel is owned by this adapter; another block caller, a
 * Host RPC consumer or an out-of-framing consumer must not share it. The caller
 * also owns the IRQ route, runs in a task context with interrupts enabled and
 * keeps the Host pumping. Those are preconditions of yan_fs_block_init(), which
 * can only observe the standing channel state. */
typedef struct {
    bool initialized;
    bool busy;
    bool failed;
    uint16_t next_tag;
    uint8_t pending_op;
    uint16_t pending_tag;
    uint32_t pending_lba;
} YanFsBlockAdapter;

/* Zero-initialise the adapter before the first call. The channel must be ready
 * (HOST_READY), hold at least one block per request (max_count >= 1), have both
 * rings empty and no overflow latched. The call sends no request and moves no
 * ring pointer. An already-initialised busy or failed adapter is not reinitialised
 * to clear that state: it returns BUSY or FAULTED. */
YanFsResult yan_fs_block_init(YanFsBlockAdapter *);

/* The three synchronous callbacks, with context pointing at the adapter. A
 * callback on an uninitialised or failed adapter returns YAN_FS_IO_PROTOCOL. */
YanFsBlockIo yan_fs_block_backend(YanFsBlockAdapter *);

#endif
