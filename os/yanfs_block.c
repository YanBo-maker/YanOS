/* Guest adapter that maps the YanFS block callbacks onto the block protocol and
 * the cooperative runtime.
 *
 * Contract: docs/specs/0021-yanfs.md "Guest 适配公开接口" and "Guest 适配与实际
 * 硬件约束". Framing is docs/specs/0018-block-protocol.md; the channel, its
 * flow control and the doorbell are docs/specs/0014-host-transport-channel.md.
 *
 * The adapter is deliberately small: it builds one request, submits it whole,
 * waits on a read-only predicate for one complete response, consumes it, and
 * pairs it against what it sent. The two rules that shape the code:
 *
 *   no blind waiting  there is no "G2H space became available" interrupt, so a
 *   submit that returns AGAIN or INVALID is a channel-contract failure and the
 *   adapter fails closed instead of waiting for something that cannot wake it.
 *
 *   a predicate that cannot hang  the predicate derives the frame length from
 *   the real header. A header that can never describe a frame this ring holds
 *   (unknown op, non-zero reserved, oversized count) is reported ready so the
 *   main flow consumes it and reports PROTOCOL, rather than waiting forever for
 *   a payload that will never come. A wrong tag or op never decides sleep;
 *   pairing is checked only after the whole legitimate frame is consumed. */
#include "yanfs_block.h"

#include "block.h"
#include "platform.h"
#include "task.h"

#include <stddef.h>

static uint32_t load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads the i-th byte of the response stream without consuming it. The consumer
 * position is H2G_TAIL and the index is taken modulo the ring size, so a header
 * that crosses the wrap point reads back contiguous (0018 rule 2). This is a
 * pure read: no ring position is advanced here. */
static uint8_t peek_response(uint32_t index)
{
    const uint32_t size = yan_os_transport_ring_size();
    const volatile uint8_t *ring =
        (const volatile uint8_t *)(uintptr_t)(yan_os_transport_ring_base() + size);
    return ring[yan_os_ring_index(yan_os_transport_h2g_tail(), size, index)];
}

/* The task_wait predicate. It must stay short, read-only and free of wait /
 * yield / exit: the runtime asks it with interrupts off. It decides only
 * "is a complete, plausible response frame available, or is the header
 * unusable", never "does this response belong to my request": tag matching is
 * not a sleep condition.
 *
 * The shape checks come first, before any length is derived. A success READ or
 * WRITE must claim count 1 (the adapter only ever sends one block), a CAPACITY
 * success must be lba 0 / count 0, an error frame must claim count 0, and
 * reserved must be zero. A header that violates any of these is reported ready
 * so the main flow consumes it and reports PROTOCOL; without this, a CAPACITY
 * header claiming 8 payload bytes or a READ success claiming two blocks would
 * make the task sleep for a payload that can never come. */
static int response_ready(void *context)
{
    (void)context;
    const uint32_t size = yan_os_transport_ring_size();
    if (size == 0u) {
        return 1; /* unusable channel: let the main flow consume and report */
    }
    const uint32_t available = yan_os_transport_h2g_available();
    if (available < YAN_OS_BLOCK_HEADER_SIZE) {
        return 0;
    }
    uint8_t header[YAN_OS_BLOCK_HEADER_SIZE];
    for (uint32_t i = 0; i < YAN_OS_BLOCK_HEADER_SIZE; ++i) {
        header[i] = peek_response(i);
    }
    if (load_le32(header + 12) != 0u) {
        return 1; /* reserved must be zero */
    }
    const uint8_t op = header[0];
    const uint8_t status = header[1];
    const uint32_t lba = load_le32(header + 4);
    const uint32_t count = load_le32(header + 8);
    const bool success = status == YAN_OS_BLOCK_STATUS_OK;
    uint64_t payload;
    switch (op) {
    case YAN_OS_BLOCK_OP_READ:
        if (success) {
            if (count != 1u) {
                return 1; /* the adapter never sends a multi-block read */
            }
            payload = YAN_OS_BLOCK_BLOCK_SIZE;
        } else {
            if (count != 0u) {
                return 1; /* a failure carries no data */
            }
            payload = 0u;
        }
        break;
    case YAN_OS_BLOCK_OP_WRITE:
        if (success ? count != 1u : count != 0u) {
            return 1;
        }
        payload = 0u;
        break;
    case YAN_OS_BLOCK_OP_CAPACITY:
        if (success) {
            if (count != 0u || lba != 0u) {
                return 1; /* a capacity success has lba 0 and count 0 */
            }
            payload = 8u;
        } else {
            if (count != 0u) {
                return 1;
            }
            payload = 0u;
        }
        break;
    default:
        return 1; /* unknown op: header is the whole frame */
    }
    const uint64_t total = (uint64_t)YAN_OS_BLOCK_HEADER_SIZE + payload;
    if (total > (uint64_t)(size - 1u)) {
        return 1; /* no frame this ring could ever hold; do not wait for it */
    }
    return (uint64_t)available >= total;
}

/* True for a header whose shape 0018 defines, writing its legal payload size.
 * This mirrors response_ready's shape rules but reports only "this frame is
 * legitimate", never "consume it now": an unknown op, non-zero reserved, or a
 * count/status combination outside the protocol returns false and is left for
 * take to handle exactly as before (a malformed frame may still be dropped by
 * take, or left in the ring for a fresh process). */
static bool response_legal_payload(uint64_t *payload_out)
{
    if (yan_os_transport_h2g_available() < YAN_OS_BLOCK_HEADER_SIZE) {
        return false;
    }
    uint8_t header[YAN_OS_BLOCK_HEADER_SIZE];
    for (uint32_t i = 0; i < YAN_OS_BLOCK_HEADER_SIZE; ++i) {
        header[i] = peek_response(i);
    }
    if (load_le32(header + 12) != 0u) {
        return false;
    }
    const uint8_t op = header[0];
    const uint8_t status = header[1];
    const uint32_t lba = load_le32(header + 4);
    const uint32_t count = load_le32(header + 8);
    const bool success = status == YAN_OS_BLOCK_STATUS_OK;
    switch (op) {
    case YAN_OS_BLOCK_OP_READ:
        if (success ? count != 1u : count != 0u) {
            return false;
        }
        *payload_out = success ? (uint64_t)YAN_OS_BLOCK_BLOCK_SIZE : 0u;
        return true;
    case YAN_OS_BLOCK_OP_WRITE:
        if (success ? count != 1u : count != 0u) {
            return false;
        }
        *payload_out = 0u;
        return true;
    case YAN_OS_BLOCK_OP_CAPACITY:
        if (success) {
            if (count != 0u || lba != 0u) {
                return false;
            }
            *payload_out = 8u;
        } else {
            if (count != 0u) {
                return false;
            }
            *payload_out = 0u;
        }
        return true;
    default:
        return false;
    }
}

static YanFsIoResult finish_ok(YanFsBlockAdapter *adapter)
{
    adapter->busy = false;
    adapter->pending_op = 0u;
    adapter->pending_tag = 0u;
    adapter->pending_lba = 0u;
    return YAN_FS_IO_OK;
}

static YanFsIoResult finish_io(YanFsBlockAdapter *adapter)
{
    adapter->busy = false;
    adapter->pending_op = 0u;
    adapter->pending_tag = 0u;
    adapter->pending_lba = 0u;
    return YAN_FS_IO_ERROR;
}

static YanFsIoResult finish_protocol(YanFsBlockAdapter *adapter)
{
    adapter->failed = true;
    adapter->busy = false;
    adapter->pending_op = 0u;
    adapter->pending_tag = 0u;
    adapter->pending_lba = 0u;
    return YAN_FS_IO_PROTOCOL;
}

/* Drain space for a complete legitimate response whose payload is larger than
 * the caller's take buffer (a READ or CAPACITY frame answering a request that
 * asked for no such payload). The transport channel is owned exclusively by this
 * one adapter (0021 "整个 transport 由该 adapter 独占"), so one private 4 KiB
 * block is enough to take any legal frame whole; it is not a second filesystem
 * cache and the core's metadata/scratch buffers are untouched. */
static uint8_t discard_block[YAN_OS_BLOCK_BLOCK_SIZE];

static YanFsIoResult run_request(YanFsBlockAdapter *adapter, uint8_t op,
                                 uint32_t lba, const uint8_t *submit_data,
                                 uint8_t *take_buffer, uint32_t take_capacity)
{
    if (adapter == NULL) {
        return YAN_FS_IO_PROTOCOL;
    }
    if (!adapter->initialized || adapter->failed) {
        return YAN_FS_IO_PROTOCOL;
    }
    if (adapter->busy) {
        /* A reentrant call would interleave two requests on one channel. */
        adapter->failed = true;
        return YAN_FS_IO_PROTOCOL;
    }
    adapter->busy = true;

    /* Standing configuration and the overflow latch are re-checked on every
     * request, not only at init. */
    if (!yan_os_transport_host_ready() ||
        (yan_os_transport_status() & YAN_OS_TRANSPORT_STATUS_OVERFLOW_DETECTED) != 0u ||
        yan_os_block_max_count() < 1u) {
        return finish_protocol(adapter);
    }

    YanOsBlockRequest request;
    request.op = op;
    request.flags = 0u;
    request.tag = adapter->next_tag;
    request.lba = lba;
    request.count = op == YAN_OS_BLOCK_OP_CAPACITY ? 0u : 1u;

    if (yan_os_block_submit(&request, submit_data) != 0) {
        /* AGAIN is the channel saying the whole frame did not fit even though
         * the standing check said it should; INVALID is a layer bug. Neither
         * can be waited out: no G2H-space interrupt exists. */
        return finish_protocol(adapter);
    }
    adapter->pending_op = op;
    adapter->pending_tag = request.tag;
    adapter->pending_lba = lba;
    adapter->next_tag = (uint16_t)(adapter->next_tag + 1u);

    /* Fast path first, then block. The waker only makes this task runnable when
     * the predicate is satisfied; after it returns the predicate is checked
     * again, which is the "wait 醒后复检" rule. */
    while (response_ready(adapter) == 0) {
        yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, response_ready, adapter);
    }

    /* 0021 requires every complete legitimate frame to be taken and only then
     * paired, whatever its payload. The caller buffer reflects the request
     * (0 bytes for a write, 8 for a capacity query, 4096 for a read), so a
     * legitimate response that belongs to a different op can carry more bytes
     * than it holds. Point take at the private discard block in exactly that
     * case. A capacity success arriving for the adapter's own capacity query
     * still fits its 8-byte buffer, so its payload is decoded normally. */
    uint64_t legal_payload = 0u;
    if (response_legal_payload(&legal_payload) &&
        legal_payload > (uint64_t)take_capacity) {
        take_buffer = discard_block;
        take_capacity = (uint32_t)sizeof discard_block;
    }

    YanOsBlockResponse response;
    uint32_t payload_length = 0u;
    int taken = yan_os_block_take(&response, take_buffer, take_capacity,
                                  &payload_length);
    if (taken != 0) {
        /* MALFORMED was consumed by take and left the byte stream aligned;
         * AGAIN would mean the predicate lied; INVALID means a buffer or
         * channel problem. None is recoverable for pairing, so fail closed. */
        return finish_protocol(adapter);
    }
    if (response.op != adapter->pending_op ||
        response.tag != adapter->pending_tag ||
        response.lba != adapter->pending_lba) {
        /* The frame was consumed by take; only now is it rejected. A wrong tag
         * or op must never keep the predicate asleep. */
        return finish_protocol(adapter);
    }
    if (response.status == YAN_OS_BLOCK_STATUS_DEVICE) {
        if (response.count != 0u || payload_length != 0u) {
            return finish_protocol(adapter); /* error shape must match */
        }
        return finish_io(adapter);
    }
    if (response.status != YAN_OS_BLOCK_STATUS_OK) {
        return finish_protocol(adapter); /* 1, 3 and anything unknown */
    }
    switch (op) {
    case YAN_OS_BLOCK_OP_READ:
        if (response.count != 1u || payload_length != YAN_OS_BLOCK_BLOCK_SIZE) {
            return finish_protocol(adapter);
        }
        break;
    case YAN_OS_BLOCK_OP_WRITE:
        if (response.count != 1u || payload_length != 0u) {
            return finish_protocol(adapter);
        }
        break;
    default: /* capacity */
        if (response.count != 0u || payload_length != 8u) {
            return finish_protocol(adapter);
        }
        break;
    }
    return finish_ok(adapter);
}

static YanFsIoResult adapter_capacity(void *context, uint64_t *blocks)
{
    if (blocks == NULL) {
        return YAN_FS_IO_PROTOCOL;
    }
    uint8_t payload[8];
    YanFsIoResult result = run_request(context, YAN_OS_BLOCK_OP_CAPACITY, 0u, NULL,
                                       payload, (uint32_t)sizeof payload);
    if (result != YAN_FS_IO_OK) {
        return result;
    }
    *blocks = yan_os_block_count_from_bytes(payload);
    return YAN_FS_IO_OK;
}

static YanFsIoResult adapter_read_block(void *context, uint32_t lba,
                                        uint8_t out[4096])
{
    return run_request(context, YAN_OS_BLOCK_OP_READ, lba, NULL, out,
                       YAN_OS_BLOCK_BLOCK_SIZE);
}

static YanFsIoResult adapter_write_block(void *context, uint32_t lba,
                                         const uint8_t data[4096])
{
    return run_request(context, YAN_OS_BLOCK_OP_WRITE, lba, data, NULL, 0u);
}

YanFsResult yan_fs_block_init(YanFsBlockAdapter *adapter)
{
    if (adapter == NULL) {
        return YAN_FS_INVALID;
    }
    if (adapter->initialized) {
        if (adapter->busy) {
            return YAN_FS_BUSY;
        }
        if (adapter->failed) {
            return YAN_FS_FAULTED;
        }
    }

    /* Channel preconditions, read-only: no request is sent and no ring pointer
     * is moved. The IRQ route, task context and interrupt enable are caller
     * preconditions this function has no way to observe. */
    if (!yan_os_transport_host_ready()) {
        return YAN_FS_PROTOCOL;
    }
    if ((yan_os_transport_status() & YAN_OS_TRANSPORT_STATUS_OVERFLOW_DETECTED) != 0u) {
        return YAN_FS_PROTOCOL;
    }
    if (yan_os_block_max_count() < 1u) {
        return YAN_FS_PROTOCOL;
    }
    if (yan_os_transport_g2h_head() != yan_os_transport_g2h_tail()) {
        return YAN_FS_PROTOCOL; /* the request ring must start empty */
    }
    if (yan_os_transport_h2g_available() != 0u) {
        return YAN_FS_PROTOCOL; /* the response ring must start empty */
    }

    adapter->initialized = true;
    adapter->busy = false;
    adapter->failed = false;
    adapter->next_tag = 0u;
    adapter->pending_op = 0u;
    adapter->pending_tag = 0u;
    adapter->pending_lba = 0u;
    return YAN_FS_OK;
}

YanFsBlockIo yan_fs_block_backend(YanFsBlockAdapter *adapter)
{
    YanFsBlockIo io;
    io.context = adapter;
    io.capacity = adapter_capacity;
    io.read_block = adapter_read_block;
    io.write_block = adapter_write_block;
    return io;
}
