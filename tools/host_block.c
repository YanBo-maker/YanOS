/* Host side of the block request protocol. The contract is tools/host_block.h
 * and docs/specs/0018-block-protocol.md.
 *
 * This is a host program, not a device: the pump is called between Guest
 * instructions (tools/yan_run.c) and answers whatever complete request the
 * guest-to-host ring holds. The four rules it exists to keep are the ones the
 * spec calls the easiest to get wrong:
 *
 *   1. fewer than 16 bytes in the ring are not interpreted at all, not even the
 *      op, because a header that has not fully arrived is not a header;
 *   2. a frame is consumed by its full length, including a frame that fails
 *      validation - the payload of a rejected write is still in the stream, and
 *      leaving it there would make every later request parse as garbage;
 *   3. a count whose product with the block size does not fit the ring is
 *      refused at once instead of being awaited forever;
 *   4. a response is written in full and published in one step, so the
 *      interrupt that announces it never refers to a frame the Guest cannot
 *      read to its end.
 *
 * A request that has not fully arrived is left where it is, positions
 * untouched, and the pump returns: it never blocks and never waits.
 *
 * All byte order here is little-endian, field by field. The header is not overlaid
 * on a struct, so padding and host endianness cannot enter the protocol. */
#include "host_block.h"

#include <string.h>

#include "yan/ram.h"
#include "yan/transport.h"

#define YAN_HOST_BLOCK_HEADER_SIZE UINT32_C(16)

/* docs/specs/0018: the request header's op byte. */
#define YAN_HOST_BLOCK_OP_READ UINT8_C(1)
#define YAN_HOST_BLOCK_OP_WRITE UINT8_C(2)
#define YAN_HOST_BLOCK_OP_CAPACITY UINT8_C(3)
/* ... and the response header's status byte. */
#define YAN_HOST_BLOCK_STATUS_OK UINT8_C(0)
#define YAN_HOST_BLOCK_STATUS_INVALID UINT8_C(1)
#define YAN_HOST_BLOCK_STATUS_DEVICE UINT8_C(2)
#define YAN_HOST_BLOCK_STATUS_UNSUPPORTED UINT8_C(3)

/* The two rings are adjacent halves of one region that starts at the
 * transport's ring_base (0014). `g2h` and `h2g` are offsets into the RAM the
 * pump was handed, which is why the pump needs the address RAM was initialised
 * with as well as the ring's address. */
typedef struct {
    uint8_t *data;
    uint32_t ring_size;
    uint32_t mask;
    uint32_t g2h;
    uint32_t h2g;
} RingView;

/* False when the channel has no usable ring, or when the ring region does not
 * lie inside the RAM the caller handed over. The pump reports "nothing to do"
 * for both rather than dereferencing a window it was not given. */
static int ring_view_init(RingView *view, const YanTransport *transport,
                          const YanRam *ram, uint32_t ram_base)
{
    if (transport == NULL || ram == NULL || ram->data == NULL) {
        return 0;
    }
    const uint32_t size = transport->ring_size;
    if (size == 0 || (size & (size - 1)) != 0) {
        return 0;
    }
    if (ram->size < 2U * (size_t)size) {
        return 0;
    }
    if (transport->ring_base < ram_base) {
        return 0;
    }
    const uint32_t offset = transport->ring_base - ram_base;
    if ((size_t)offset > ram->size - 2U * (size_t)size) {
        return 0;
    }
    view->data = ram->data;
    view->ring_size = size;
    view->mask = size - 1U;
    view->g2h = offset;
    view->h2g = offset + size;
    return 1;
}

/* Byte access by absolute ring index; the caller passes a head or a tail and an
 * offset from it, so the wrap is handled here once instead of at every call
 * site. The index is taken modulo the ring size (0018 rule 2), which is also why
 * a request header that crosses the wrap point reads back contiguous. */
static void ring_store(const RingView *view, uint32_t ring, uint32_t index,
                       const uint8_t *bytes, uint32_t count)
{
    uint8_t *target = view->data + ring;
    for (uint32_t i = 0; i < count; ++i) {
        target[(index + i) & view->mask] = bytes[i];
    }
}

static void ring_load(const RingView *view, uint32_t ring, uint32_t index,
                      uint8_t *out, uint32_t count)
{
    const uint8_t *source = view->data + ring;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = source[(index + i) & view->mask];
    }
}

static void ring_store_storage(const RingView *view, uint32_t index,
                               const uint8_t *storage, uint64_t bytes)
{
    uint8_t *target = view->data + view->h2g;
    for (uint64_t i = 0; i < bytes; ++i) {
        target[(index + (uint32_t)i) & view->mask] = storage[i];
    }
}

static void ring_load_storage(const RingView *view, uint32_t index,
                              uint8_t *storage, uint64_t bytes)
{
    const uint8_t *source = view->data + view->g2h;
    for (uint64_t i = 0; i < bytes; ++i) {
        storage[i] = source[(index + (uint32_t)i) & view->mask];
    }
}

/* ------------------------------------------------------------------ headers */

typedef struct {
    uint8_t op;
    uint8_t flags;
    uint16_t tag;
    uint32_t lba;
    uint32_t count;
    uint32_t reserved;
} RequestHeader;

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

static void store_le32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i) {
        bytes[i] = (uint8_t)(value >> (8U * i));
    }
}

static void store_le64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t i = 0; i < 8; ++i) {
        bytes[i] = (uint8_t)(value >> (8U * i));
    }
}

static void decode_request(const uint8_t bytes[16], RequestHeader *header)
{
    header->op = bytes[0];
    header->flags = bytes[1];
    header->tag = load_le16(bytes + 2);
    header->lba = load_le32(bytes + 4);
    header->count = load_le32(bytes + 8);
    header->reserved = load_le32(bytes + 12);
}

/* `count` is the blocks the response claims to have completed: the requested
 * count when the request succeeded and zero otherwise, because v1 fails as a
 * whole and never reports partial completion (0018). */
static void encode_response(uint8_t bytes[16], uint8_t op, uint8_t status,
                            uint16_t tag, uint32_t lba, uint32_t count)
{
    memset(bytes, 0, 16);
    bytes[0] = op;
    bytes[1] = status;
    bytes[2] = (uint8_t)tag;
    bytes[3] = (uint8_t)(tag >> 8);
    store_le32(bytes + 4, lba);
    store_le32(bytes + 8, count);
    /* reserved stays zero: 0018 fixes it, and a request that sets it is
     * rejected above. */
}

/* Bytes a request occupies in the guest-to-host ring.
 *
 * Only a write carries a payload; a read and a capacity query are the header
 * alone, and so is an unknown op, because nothing else is defined for it.
 *
 * Returns 0, with *length set to the header alone, when the header describes a
 * write whose frame cannot fit the ring: such a frame cannot exist, so awaiting
 * it would hang, and taking its length from an arithmetic accident (a 32-bit
 * count * 4096 that wrapped to something small) is how a stream gets shredded. */
static int request_length(const RequestHeader *header, uint32_t ring_size,
                          uint32_t *length)
{
    *length = YAN_HOST_BLOCK_HEADER_SIZE;
    if (header->op != YAN_HOST_BLOCK_OP_WRITE || header->count == 0) {
        return 1;
    }
    const uint64_t total =
        (uint64_t)YAN_HOST_BLOCK_HEADER_SIZE +
        (uint64_t)header->count * YAN_HOST_BLOCK_BLOCK_SIZE;
    if (total > (uint64_t)(ring_size - 1U)) {
        return 0;
    }
    *length = (uint32_t)total;
    return 1;
}

/* Whether the response can exist in the ring at all. A read request is 16 bytes
 * but its response is 16 + count * 4096, so a count that fits the request can
 * still leave a reply nothing could hold - the read has to be refused rather
 * than left waiting for room that can never come. */
static int response_fits(const RequestHeader *header, uint32_t ring_size)
{
    uint64_t total = YAN_HOST_BLOCK_HEADER_SIZE;
    if (header->op == YAN_HOST_BLOCK_OP_READ) {
        total += (uint64_t)header->count * YAN_HOST_BLOCK_BLOCK_SIZE;
    } else if (header->op == YAN_HOST_BLOCK_OP_CAPACITY) {
        total += 8U;
    }
    return total <= (uint64_t)(ring_size - 1U);
}

/* Bytes the response occupies. A read's payload is the reason a read request
 * can be unsendable even though its request frame is 16 bytes. */
static uint32_t response_length(uint8_t op, uint8_t status, uint32_t count)
{
    if (status != YAN_HOST_BLOCK_STATUS_OK) {
        return YAN_HOST_BLOCK_HEADER_SIZE;
    }
    if (op == YAN_HOST_BLOCK_OP_READ) {
        return YAN_HOST_BLOCK_HEADER_SIZE + count * YAN_HOST_BLOCK_BLOCK_SIZE;
    }
    if (op == YAN_HOST_BLOCK_OP_CAPACITY) {
        return YAN_HOST_BLOCK_HEADER_SIZE + 8U;
    }
    return YAN_HOST_BLOCK_HEADER_SIZE;
}

/* ---------------------------------------------------------------- interface */

YanStatus yan_host_block_init(YanHostBlock *block, uint8_t *storage,
                              uint64_t capacity_blocks)
{
    if (block == NULL || (storage == NULL && capacity_blocks != 0)) {
        return YAN_INVALID_ARGUMENT;
    }
    block->storage = storage;
    block->capacity_blocks = capacity_blocks;
    block->served = 0;
    block->fail_next = false;
    return YAN_OK;
}

uint32_t yan_host_block_service(YanHostBlock *block, YanTransport *transport,
                                YanRam *ram, uint32_t ram_base)
{
    if (block == NULL || transport == NULL) {
        return 0;
    }
    RingView view;
    if (!ring_view_init(&view, transport, ram, ram_base)) {
        return 0;
    }

    uint32_t answered = 0;
    for (;;) {
        /* Rule 1: a header that has not fully arrived is not interpreted. */
        const uint32_t available = yan_transport_host_readable(transport);
        if (available < YAN_HOST_BLOCK_HEADER_SIZE) {
            break;
        }

        uint8_t header_bytes[16];
        ring_load(&view, view.g2h, transport->g2h_tail, header_bytes, 16);
        RequestHeader header;
        decode_request(header_bytes, &header);

        uint32_t frame_length = 0;
        const int frame_fits = request_length(&header, view.ring_size, &frame_length);
        /* Rule 4: a frame that has not fully arrived is not consumed. */
        if (available < frame_length) {
            break;
        }
        uint8_t status = YAN_HOST_BLOCK_STATUS_OK;
        if (header.op != YAN_HOST_BLOCK_OP_READ && header.op != YAN_HOST_BLOCK_OP_WRITE &&
            header.op != YAN_HOST_BLOCK_OP_CAPACITY) {
            /* The op decides first: an unknown op is unsupported whatever else
             * the header says. */
            status = YAN_HOST_BLOCK_STATUS_UNSUPPORTED;
        } else if (header.flags != 0 || header.reserved != 0) {
            status = YAN_HOST_BLOCK_STATUS_INVALID;
        } else if (!frame_fits || !response_fits(&header, view.ring_size)) {
            /* The header describes a frame or a reply the ring cannot hold, so
             * it can never be received or answered: a count whose size wrapped,
             * or one too large for the ring. Reported instead of awaited. */
            status = YAN_HOST_BLOCK_STATUS_INVALID;
        } else if (header.op == YAN_HOST_BLOCK_OP_CAPACITY) {
            if (header.lba != 0 || header.count != 0) {
                status = YAN_HOST_BLOCK_STATUS_INVALID;
            }
        } else if (header.count == 0) {
            /* Zero blocks completes nothing, so it is not a request. */
            status = YAN_HOST_BLOCK_STATUS_INVALID;
        } else if ((uint64_t)header.lba + header.count > block->capacity_blocks) {
            /* Written as a 64-bit sum so `lba + count` cannot wrap past the
             * capacity check. A read response is bounded by the ring too, and
             * that is what `response_fits` above holds. */
            status = YAN_HOST_BLOCK_STATUS_INVALID;
        }

        /* The failure path has to be reachable on demand: a memory device never
         * fails by itself. Only a request that would touch the store is the one
         * the flag is for, and the flag is spent by an *answer*, not by an
         * attempt: a reply that does not fit (`break` below) means this request
         * was not answered at all, while tools/host_block.h promises the fault
         * to "the next request that touches storage". Clearing the flag here
         * would let a response ring that is momentarily full swallow the
         * injection silently. */
        const int faulted = status == YAN_HOST_BLOCK_STATUS_OK && block->fail_next &&
                            (header.op == YAN_HOST_BLOCK_OP_READ ||
                             header.op == YAN_HOST_BLOCK_OP_WRITE);
        if (faulted) {
            status = YAN_HOST_BLOCK_STATUS_DEVICE;
        }

        const uint32_t reply_length = response_length(header.op, status, header.count);
        /* Rule 4 for the other direction: the response is published in one
         * piece or not at all. When it does not fit, the request stays where it
         * is and the next call tries again once the Guest has drained. */
        if (reply_length > yan_transport_host_writable(transport)) {
            break;
        }

        /* Rule 5: the frame is consumed by its full length, exactly once, and
         * a frame that failed validation is consumed too - its bytes are in the
         * stream whatever the answer was. The position is kept first, because
         * consuming moves the tail the payload is read from. */
        const uint32_t frame_at = transport->g2h_tail;
        if (yan_transport_host_consume(transport, frame_length) != YAN_OK) {
            break;
        }

        /* The backend, reached only by a request that passed validation. */
        if (status == YAN_HOST_BLOCK_STATUS_OK && header.op == YAN_HOST_BLOCK_OP_WRITE) {
            ring_load_storage(&view, frame_at + YAN_HOST_BLOCK_HEADER_SIZE,
                              block->storage + (uint64_t)header.lba * YAN_HOST_BLOCK_BLOCK_SIZE,
                              (uint64_t)header.count * YAN_HOST_BLOCK_BLOCK_SIZE);
        }

        uint8_t reply[16];
        encode_response(reply, header.op, status, header.tag, header.lba,
                        status == YAN_HOST_BLOCK_STATUS_OK ? header.count : 0);
        const uint32_t at = transport->h2g_head;
        ring_store(&view, view.h2g, at, reply, sizeof reply);
        if (status == YAN_HOST_BLOCK_STATUS_OK && header.op == YAN_HOST_BLOCK_OP_READ) {
            ring_store_storage(&view, at + YAN_HOST_BLOCK_HEADER_SIZE,
                               block->storage + (uint64_t)header.lba * YAN_HOST_BLOCK_BLOCK_SIZE,
                               (uint64_t)header.count * YAN_HOST_BLOCK_BLOCK_SIZE);
        } else if (status == YAN_HOST_BLOCK_STATUS_OK &&
                   header.op == YAN_HOST_BLOCK_OP_CAPACITY) {
            uint8_t capacity_bytes[8];
            store_le64(capacity_bytes, block->capacity_blocks);
            ring_store(&view, view.h2g, at + YAN_HOST_BLOCK_HEADER_SIZE, capacity_bytes,
                       8);
        }

        /* Rule 4 again: the whole reply is in the ring before the head moves,
         * so the interrupt and the frame become visible together. Only here,
         * with the failure actually on its way to the Guest, is the injected
         * fault spent. */
        if (yan_transport_host_publish(transport, reply_length) != YAN_OK) {
            break;
        }
        if (faulted) {
            block->fail_next = false;
        }
        block->served++;
        ++answered;
    }
    return answered;
}
