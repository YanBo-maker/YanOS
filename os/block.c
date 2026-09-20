/* Guest side of the block request protocol. The contract is os/block.h and
 * docs/specs/0018-block-protocol.md.
 *
 * The layer does four things and nothing else: it builds the 16-byte header,
 * checks that the *whole* frame fits the guest-to-host ring before writing a
 * byte of it, waits until a whole response frame has arrived before consuming
 * any of it, and consumes a frame by its full length even when it is dropped.
 *
 * Two rules of the spec shape the code below more than the others:
 *
 *   "整帧原子性与部分到达" - fewer than 16 bytes are not interpreted, not even
 *   the op, and the incomplete part is not consumed. Framing therefore has to
 *   *look* at a header before it can decide to take it. The peek below reads
 *   ring bytes through the base and size os/platform.h exposes; it never writes
 *   a ring position. Consuming still goes through
 *   yan_os_transport_h2g_pop(), which is the only thing here that moves
 *   H2G_TAIL, so the flow-control wrapper stays the single writer of positions
 *   that 0014 requires.
 *
 *   "发送前流控" - the frame is measured before the first byte goes in. The
 *   bytes themselves are pushed by the platform helper, which is all or nothing
 *   per call; checking the total first is what makes the two calls (a write's
 *   header and its payload) one atomic frame, and it is why a request that does
 *   not fit leaves the ring exactly as it was.
 *
 * Everything multi-byte is little-endian, field by field. No header is overlaid
 * on a struct, so neither padding nor host endianness can leak into the wire
 * format. */
#include "block.h"

#include <stddef.h>

#include "platform.h"

/* The response header's second byte is the status; os/block.h names the values. */

/* Reads the i-th byte of the response stream without consuming it. The consumer
 * position is H2G_TAIL, and the index is taken modulo the ring size, so a
 * header that crosses the wrap point reads back contiguous (0018 rule 2). */
static uint8_t peek_response(uint32_t index)
{
    const uint32_t size = yan_os_transport_ring_size();
    const volatile uint8_t *ring =
        (const volatile uint8_t *)(uintptr_t)(yan_os_transport_ring_base() + size);
    return ring[yan_os_ring_index(yan_os_transport_h2g_tail(), size, index)];
}

/* Drops `count` bytes of the response stream. Used for frames that fail
 * validation: they still have to leave the ring, or the next frame is read from
 * the middle of this one and every later response is garbage (0018 rule 5). The
 * copy is chunked so dropping a 4 KiB frame costs 64 bytes of stack, not 4 KiB. */
static int drop_response(uint32_t count)
{
    uint8_t scratch[64];
    while (count > 0) {
        const uint32_t chunk =
            count < (uint32_t)sizeof scratch ? count : (uint32_t)sizeof scratch;
        if (yan_os_transport_h2g_pop(scratch, chunk) != 0) {
            return YAN_OS_BLOCK_INVALID;
        }
        count -= chunk;
    }
    return 0;
}

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

static uint16_t load_le16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t load_le32(const uint8_t *bytes)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        value |= (uint32_t)bytes[i] << (8U * i);
    }
    return value;
}

static void encode_request(uint8_t bytes[16], const YanOsBlockRequest *request)
{
    bytes[0] = request->op;
    bytes[1] = request->flags;
    put_le16(bytes + 2, request->tag);
    put_le32(bytes + 4, request->lba);
    put_le32(bytes + 8, request->count);
    /* reserved: 0018 fixes it at zero for a request the layer builds. */
    put_le32(bytes + 12, 0);
}

uint32_t yan_os_block_max_count(void)
{
    if (!yan_os_transport_host_ready()) {
        return 0;
    }
    const uint32_t size = yan_os_transport_ring_size();
    if (size <= YAN_OS_BLOCK_HEADER_SIZE + 1U) {
        return 0;
    }
    /* One cell stays free (0014), the header takes 16, and what is left holds
     * blocks. A write's frame and a read's response are the same size, so this
     * one bound covers both directions and no caller has to know which way
     * round the limit applies. */
    const uint32_t usable = size - 1U - YAN_OS_BLOCK_HEADER_SIZE;
    return usable / YAN_OS_BLOCK_BLOCK_SIZE;
}

int yan_os_block_submit(const YanOsBlockRequest *request, const uint8_t *data)
{
    if (request == NULL) {
        return YAN_OS_BLOCK_INVALID;
    }
    /* A caller bug is a caller bug whether or not a host is attached, so the
     * shape of the request is judged first. What cannot be judged without a
     * channel is the count bound, because that is what RING_SIZE decides; it
     * comes after the channel check below. */
    if (request->flags != 0) {
        return YAN_OS_BLOCK_INVALID;
    }
    if (request->op != YAN_OS_BLOCK_OP_READ && request->op != YAN_OS_BLOCK_OP_WRITE &&
        request->op != YAN_OS_BLOCK_OP_CAPACITY) {
        return YAN_OS_BLOCK_INVALID;
    }
    if (request->op == YAN_OS_BLOCK_OP_WRITE && request->count != 0 && data == NULL) {
        return YAN_OS_BLOCK_INVALID;
    }
    if (!yan_os_transport_host_ready()) {
        return YAN_OS_BLOCK_AGAIN;
    }

    uint32_t payload_bytes = 0;
    switch (request->op) {
    case YAN_OS_BLOCK_OP_READ:
        /* The response is 16 + count * 4096, so a read is bounded by the ring
         * just as a write is; `max_count` is that bound.
         *
         * A count of zero and an lba past the end are deliberately *not*
         * rejected here: 0018 answers those with status 1, and a layer that
         * refused to build the frame would hide the device's answer behind its
         * own. What this layer refuses is only what could never be a frame. */
        if (request->count > yan_os_block_max_count()) {
            return YAN_OS_BLOCK_INVALID;
        }
        break;
    case YAN_OS_BLOCK_OP_WRITE:
        if (request->count > yan_os_block_max_count()) {
            return YAN_OS_BLOCK_INVALID;
        }
        payload_bytes = request->count * YAN_OS_BLOCK_BLOCK_SIZE;
        break;
    default:
        /* A capacity query: its parameters are judged by the device. */
        break;
    }

    const uint32_t frame_bytes = YAN_OS_BLOCK_HEADER_SIZE + payload_bytes;
    /* The whole frame has to fit before the first byte is written: half a frame
     * in the ring is the one failure the protocol cannot recover from. Space
     * that is merely short right now is a condition of the channel, not a
     * protocol status - the caller yields and tries again. */
    if (yan_os_transport_g2h_space() < frame_bytes) {
        return YAN_OS_BLOCK_AGAIN;
    }

    uint8_t header[16];
    encode_request(header, request);
    if (yan_os_transport_g2h_push(header, YAN_OS_BLOCK_HEADER_SIZE) != 0) {
        return YAN_OS_BLOCK_AGAIN;
    }
    if (payload_bytes != 0) {
        /* Cannot fail: the space for both pushes was reserved above and the
         * host only ever frees more of it. */
        if (yan_os_transport_g2h_push(data, payload_bytes) != 0) {
            return YAN_OS_BLOCK_INVALID;
        }
    }
    yan_os_transport_doorbell();
    return 0;
}

int yan_os_block_take(YanOsBlockResponse *response, uint8_t *data,
                      uint32_t capacity, uint32_t *length)
{
    if (response == NULL) {
        return YAN_OS_BLOCK_INVALID;
    }
    if (length != NULL) {
        *length = 0;
    }
    if (!yan_os_transport_host_ready()) {
        return YAN_OS_BLOCK_INVALID;
    }

    /* Rule 1: fewer than 16 bytes are not interpreted, not even the op. */
    const uint32_t available = yan_os_transport_h2g_available();
    if (available < YAN_OS_BLOCK_HEADER_SIZE) {
        return YAN_OS_BLOCK_AGAIN;
    }

    uint8_t peeked[16];
    for (uint32_t i = 0; i < sizeof peeked; ++i) {
        peeked[i] = peek_response(i);
    }
    const uint8_t op = peeked[0];
    const uint8_t status = peeked[1];
    /* The payload stays 64 bits wide until the frame is known to fit the ring:
     * a count whose product with the block size wraps 32 bits is exactly the
     * case that must be refused, and narrowing first would turn it into a
     * plausible frame length. */
    uint64_t payload = 0;
    int malformed = 0;
    switch (op) {
    case YAN_OS_BLOCK_OP_READ:
        /* A failure carries no data at all: 0018 says a response with a
         * non-zero status is the header and nothing else. */
        payload = status == YAN_OS_BLOCK_STATUS_OK
                      ? (uint64_t)load_le32(peeked + 8) * YAN_OS_BLOCK_BLOCK_SIZE
                      : 0;
        break;
    case YAN_OS_BLOCK_OP_WRITE:
        payload = 0;
        break;
    case YAN_OS_BLOCK_OP_CAPACITY:
        payload = status == YAN_OS_BLOCK_STATUS_OK ? 8U : 0U;
        break;
    default:
        /* Nothing but the header is defined for an op this layer does not
         * know, so the header is the whole frame. */
        malformed = 1;
        payload = 0;
        break;
    }
    if (load_le32(peeked + 12) != 0) {
        malformed = 1;
    }

    const uint64_t total = (uint64_t)YAN_OS_BLOCK_HEADER_SIZE + payload;
    if (total > (uint64_t)(yan_os_transport_ring_size() - 1U)) {
        /* The count cannot describe a frame this ring could ever hold, so
         * waiting for it would wait forever. The header is consumed and the
         * frame is reported as malformed; treating the wrapped product as a
         * frame length is what would shred the stream. */
        return drop_response(YAN_OS_BLOCK_HEADER_SIZE) == 0 ? YAN_OS_BLOCK_MALFORMED
                                                            : YAN_OS_BLOCK_INVALID;
    }
    /* Rule 4: nothing is consumed until the whole frame is there. */
    if (available < total) {
        return YAN_OS_BLOCK_AGAIN;
    }
    /* A buffer that cannot hold the payload is the caller's problem and stays
     * the caller's problem: consuming the frame would destroy it, so the frame
     * is left in the ring for the next call with a larger buffer. */
    if (payload > (uint64_t)capacity || (payload != 0 && data == NULL)) {
        return YAN_OS_BLOCK_INVALID;
    }

    if (malformed != 0) {
        return drop_response((uint32_t)total) == 0 ? YAN_OS_BLOCK_MALFORMED
                                                   : YAN_OS_BLOCK_INVALID;
    }

    /* The frame is complete and wanted: consume it, header first, and the
     * payload straight into the caller's buffer. */
    uint8_t taken[16];
    if (yan_os_transport_h2g_pop(taken, YAN_OS_BLOCK_HEADER_SIZE) != 0) {
        return YAN_OS_BLOCK_INVALID;
    }
    if (payload != 0 &&
        yan_os_transport_h2g_pop(data, (uint32_t)payload) != 0) {
        return YAN_OS_BLOCK_INVALID;
    }
    /* The delivered header is the one that was consumed, not the one that was
     * peeked at: they are the same frame by construction, and reading them back
     * from the consumed bytes is what makes that true rather than assumed. */
    response->op = taken[0];
    response->status = taken[1];
    response->tag = load_le16(taken + 2);
    response->lba = load_le32(taken + 4);
    response->count = load_le32(taken + 8);
    if (length != NULL) {
        *length = (uint32_t)payload;
    }
    return 0;
}
