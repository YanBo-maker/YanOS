#ifndef YAN_OS_BLOCK_H
#define YAN_OS_BLOCK_H

#include <stdint.h>

/* Guest side of the block request protocol in
 * docs/specs/0018-block-protocol.md: the framing layer that turns "submit one
 * request" and "take one response" into bytes on the host transport channel
 * (docs/specs/0014-host-transport-channel.md).
 *
 * What lives here and what does not:
 *
 *   here      the 16-byte header layout, the little-endian encoding, the
 *             whole-frame flow-control check before anything is written, the
 *             rule that fewer than 16 bytes are never interpreted, and the rule
 *             that a frame is consumed by its full length even when it fails
 *             validation.
 *   above     block numbers, capacity and what a block means. The layer counts
 *             blocks because 4096 is the size YanFS will use; it does not know
 *             what is in them.
 *   below     the channel. Every ring position is advanced by the accessors in
 *             os/platform.h and never by this file.
 *
 * The caller is expected to alternate: submit one request, then take its
 * response before submitting the next. The protocol has a single queue and
 * answers in arrival order, so a caller that keeps two requests in flight would
 * have to pair them itself and gains nothing in v1. */

#define YAN_OS_BLOCK_BLOCK_SIZE UINT32_C(4096)
#define YAN_OS_BLOCK_HEADER_SIZE UINT32_C(16)

#define YAN_OS_BLOCK_OP_READ UINT8_C(1)
#define YAN_OS_BLOCK_OP_WRITE UINT8_C(2)
#define YAN_OS_BLOCK_OP_CAPACITY UINT8_C(3)

/* status, the response header's second byte. */
#define YAN_OS_BLOCK_STATUS_OK UINT8_C(0)
#define YAN_OS_BLOCK_STATUS_INVALID UINT8_C(1)
#define YAN_OS_BLOCK_STATUS_DEVICE UINT8_C(2)
#define YAN_OS_BLOCK_STATUS_UNSUPPORTED UINT8_C(3)

/* Returns of yan_os_block_submit() and yan_os_block_take(). The two failures
 * are kept apart because they call for different reactions: AGAIN is a
 * condition of the channel that changes by itself (retry), INVALID is a caller
 * or stream error that retrying cannot fix. */
#define YAN_OS_BLOCK_AGAIN (-1)
#define YAN_OS_BLOCK_INVALID (-2)
/* take() reports a frame that was consumed but could not be delivered. */
#define YAN_OS_BLOCK_MALFORMED (-3)

typedef struct {
    uint8_t op;
    uint8_t flags;
    uint16_t tag;
    uint32_t lba;
    uint32_t count;
} YanOsBlockRequest;

typedef struct {
    uint8_t op;
    uint8_t status;
    uint16_t tag;
    uint32_t lba;
    /* Blocks actually completed. Zero whenever status is not OK, because v1
     * fails as a whole and never reports partial completion. */
    uint32_t count;
} YanOsBlockResponse;

/* Blocks one request may carry in the channel as it is configured now: the
 * smallest of what the request frame needs and what its response frame needs,
 * both against RING_SIZE - 1. A read request is 16 bytes but its response is
 * 16 + count * 4096, so the response is what limits a read; a write is the
 * other way round. Returns 0 when no channel is configured or when not even one
 * block fits, which is a platform configuration fault the caller has to treat
 * as "this request cannot be sent", not as "retry later". */
uint32_t yan_os_block_max_count(void);

/* Submit one request. For a write, data must hold count * 4096 bytes; for a
 * read or a capacity query it is not read at all and may be NULL.
 *
 * The whole frame - the header and, for a write, the payload - is checked
 * against the guest-to-host ring before the first byte is written, so a request
 * that does not fit leaves the ring exactly as it was. Half a frame in the ring
 * is the one failure mode the protocol cannot recover from: the host would
 * parse the payload as the next header and every later response would be
 * garbage.
 *
 * Returns 0 when the frame was published and the host rang; YAN_OS_BLOCK_AGAIN
 * when the channel is not ready yet or the whole frame does not fit right now
 * (nothing was written, the caller yields and retries); YAN_OS_BLOCK_INVALID
 * when the layer cannot build a frame at all - an unknown op, a non-zero flags,
 * count > yan_os_block_max_count(), or a write of at least one block without a
 * data buffer.
 *
 * A count of zero and an lba outside the device are *not* rejected here: they
 * are legal frames and 0018 answers them with status 1, so the caller sees the
 * device's answer instead of the layer's opinion of it. */
int yan_os_block_submit(const YanOsBlockRequest *request, const uint8_t *data);

/* Take one response if a complete one has arrived.
 *
 * A frame is never delivered in pieces: while the host-to-guest ring holds less
 * than a whole frame, this returns YAN_OS_BLOCK_AGAIN, writes nothing through
 * any output and consumes nothing. `response` and `data` are untouched unless
 * the return is 0.
 *
 * `data` receives the payload of a successful read, or the 8-byte block count
 * of a successful capacity query in little-endian order (use
 * yan_os_block_count_from_bytes() to decode it). capacity is how many bytes
 * `data` can hold; a frame whose payload does not fit is not consumed and
 * YAN_OS_BLOCK_INVALID is returned, so the caller may retry with a larger
 * buffer rather than lose the frame.
 *
 * Returns 0 when a response was delivered (with *length set to the payload
 * size), YAN_OS_BLOCK_AGAIN when no complete frame is there yet,
 * YAN_OS_BLOCK_INVALID for a NULL argument, an unconfigured channel, or a
 * payload larger than capacity, and YAN_OS_BLOCK_MALFORMED when a frame that
 * violates the header rules was consumed by its full length and dropped. The
 * malformed case is not a reason to stop: the byte stream stays aligned, so the
 * next call reads the next frame. */
int yan_os_block_take(YanOsBlockResponse *response, uint8_t *data,
                      uint32_t capacity, uint32_t *length);

/* The capacity query's payload is a little-endian u64, which is wider than the
 * machine's natural word here; this is the one place the split is decoded.
 *
 * The two halves are built as 32-bit words and combined with a shift by a
 * constant on purpose. Whether a 64-bit shift by a variable amount needs a
 * helper depends on the optimizer: this toolchain emits no __ashldi3 for the
 * loop that would do it in one step at -O0, -O1 and -O2, but calls it at -Os,
 * and a freestanding Guest links no libgcc to answer with. The form below does
 * not depend on that choice. (Measured with `riscv64-linux-gnu-nm -u` on a
 * translation unit shifting a u64 by 8 * i.) */
static inline uint64_t yan_os_block_count_from_bytes(const uint8_t bytes[8])
{
    uint32_t low = 0;
    uint32_t high = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        low |= (uint32_t)bytes[i] << (8u * i);
        high |= (uint32_t)bytes[4 + i] << (8u * i);
    }
    return ((uint64_t)high << 32) | (uint64_t)low;
}

#endif
