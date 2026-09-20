/* Guest self-check for os/block.c, the frame layer of
 * docs/specs/0018-block-protocol.md.
 *
 * The program is the executable form of the spec's VERIFY list as seen from
 * inside the Guest: it submits requests through yan_os_block_submit(), takes
 * responses through yan_os_block_take() and asserts what came back. A failed
 * expectation prints a ":FAIL:" line on the UART and ends the run with `tohost`
 * set to the high bit plus the check number, so a runner can tell a detected
 * defect (an assertion) apart from a hang or a crash, and the run is never read
 * as a pass: `tohost` 1 is the only pass code.
 *
 * One scenario is compiled in per image (-DYAN_BLOCK_SCENARIO=...), because
 * each needs a different shape of the *Host* end, which tests/guest/
 * block_drive.c scripts: a whole response in one publish, a response published
 * in pieces, a ring position rotated so a frame crosses the wrap point, an
 * injected malformed frame, and a Host that never answers.
 *
 * What the Guest cannot see is asserted by the driver instead, through the
 * report globals below: the driver reads them out of RAM and checks that the
 * Guest never moved its tail while a frame was incomplete, never delivered a
 * half frame to a caller and never relied on the interrupt line for framing.
 * The split scenarios also check the report against what the driver actually
 * paced, so "the Guest saw a partial frame" is a fact and not a hope.
 *
 * The storage pattern is restated in tests/guest/block_unit.c and
 * tests/guest/block_drive.c: a drift between the copies fails a read check
 * rather than cancelling out.
 */
#include <stdint.h>

#include "block.h"
#include "guest.h"
#include "platform.h"

#define SCENARIO_CAPACITY 1
#define SCENARIO_READ 2
#define SCENARIO_WRITE 3
#define SCENARIO_PARAMS 4
#define SCENARIO_ORDER 5
#define SCENARIO_FAULT 6
#define SCENARIO_FLOW_SHORT 7
#define SCENARIO_FLOW_RETRY 8
#define SCENARIO_LIMIT 9
#define SCENARIO_SPLIT 10
#define SCENARIO_MALFORMED 11
#define SCENARIO_NO_HOST 12

#ifndef YAN_BLOCK_SCENARIO
#define YAN_BLOCK_SCENARIO SCENARIO_CAPACITY
#endif
#ifndef YAN_BLOCK_SCENARIO_NAME
#define YAN_BLOCK_SCENARIO_NAME "unnamed"
#endif
/* The capacity tests/guest/block_drive.c attaches, and the geometry yan_run
 * places the rings with. Both are restated so a drift shows up here. */
#ifndef YAN_BLOCK_EXPECT_BLOCKS
#define YAN_BLOCK_EXPECT_BLOCKS 8
#endif
#ifndef YAN_BLOCK_EXPECT_RING_SIZE
#define YAN_BLOCK_EXPECT_RING_SIZE UINT32_C(8192)
#endif

/* ------------------------------------------------------------ report globals
 *
 * Read by tests/guest/block_drive.c out of RAM after the run. They are the
 * Guest's half of the framing evidence: "while take() said AGAIN, nothing
 * moved and nothing was written through my outputs". */
volatile uint32_t yan_block_check_polls;
volatile uint32_t yan_block_check_partial_seen;
volatile uint32_t yan_block_check_partial_payload_seen;
volatile uint32_t yan_block_check_tail_moved;
volatile uint32_t yan_block_check_output_touched;
volatile uint32_t yan_block_check_frame_wrapped;
volatile uint32_t yan_block_check_checks;

/* ------------------------------------------------------------------ console */

static void emit_text(const char *text)
{
    for (const char *at = text; *at != '\0'; ++at) {
        /* Without a terminal the device refuses every byte; the bound keeps a
         * diagnostic from becoming an infinite loop. */
        uint32_t spins = 0;
        while (!yan_os_uart_tx_ready() && spins < 100000U) {
            ++spins;
        }
        (void)yan_os_uart_put((uint8_t)*at);
    }
}

static void emit_u32(uint32_t value)
{
    char digits[11];
    uint32_t at = sizeof digits;
    digits[--at] = '\0';
    do {
        digits[--at] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0);
    emit_text(digits + at);
}

/* -------------------------------------------------------------- assertions */

static uint32_t checks;

static void fail(const char *name)
{
    emit_text(":FAIL: ");
    emit_text(YAN_BLOCK_SCENARIO_NAME);
    emit_text(": ");
    emit_text(name);
    emit_text(" (check ");
    emit_u32(checks);
    emit_text(")\n");
    /* The high bit keeps a failing code from ever reading as the pass code 1. */
    guest_finish(UINT32_C(0x80000000) | (checks & UINT32_C(0x7fffffff)));
}

static void expect(int condition, const char *name)
{
    ++checks;
    yan_block_check_checks = checks;
    if (!condition) {
        fail(name);
    }
}

static void expect_u32(uint32_t actual, uint32_t expected, const char *name)
{
    ++checks;
    yan_block_check_checks = checks;
    if (actual == expected) {
        return;
    }
    emit_text(":FAIL: ");
    emit_text(YAN_BLOCK_SCENARIO_NAME);
    emit_text(": ");
    emit_text(name);
    emit_text(": expected ");
    emit_u32(expected);
    emit_text(", got ");
    emit_u32(actual);
    emit_text("\n");
    guest_finish(UINT32_C(0x80000000) | (checks & UINT32_C(0x7fffffff)));
}

/* ------------------------------------------------------------------ content */

static uint8_t read_buffer[YAN_OS_BLOCK_BLOCK_SIZE];
static uint8_t write_buffer[YAN_OS_BLOCK_BLOCK_SIZE];
static uint8_t frame_buffer[YAN_OS_BLOCK_BLOCK_SIZE + YAN_OS_BLOCK_HEADER_SIZE];

static uint8_t pattern(uint32_t lba, uint32_t offset)
{
    return (uint8_t)((lba * 37U + offset * 11U + (offset >> 8) * 5U) & 0xffU);
}

/* What this program writes when the drive has to be able to tell "the write
 * landed" from "the block already held that content": the drive's pattern,
 * inverted, so tests/guest/block_drive.c can compare byte for byte against
 * --expect-written. */
static void fill_written(uint8_t *out, uint32_t lba)
{
    for (uint32_t at = 0; at < YAN_OS_BLOCK_BLOCK_SIZE; ++at) {
        out[at] = (uint8_t)(pattern(lba, at) ^ 0xffU);
    }
}

/* The same comparison for the blocks this program wrote itself: see
 * fill_written above. */
static int matches_written(const uint8_t *buffer, uint32_t lba)
{
    for (uint32_t at = 0; at < YAN_OS_BLOCK_BLOCK_SIZE; ++at) {
        if (buffer[at] != (uint8_t)(pattern(lba, at) ^ 0xffU)) {
            return 0;
        }
    }
    return 1;
}

static int matches_pattern(const uint8_t *buffer, uint32_t lba)
{
    for (uint32_t at = 0; at < YAN_OS_BLOCK_BLOCK_SIZE; ++at) {
        if (buffer[at] != pattern(lba, at)) {
            return 0;
        }
    }
    return 1;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

/* A frame a Guest could have built by hand, used by the flow-control scenarios
 * to leave one legal request in the ring without ringing the bell. */
static void build_frame(uint8_t *out, uint8_t op, uint16_t tag, uint32_t lba,
                        uint32_t count, const uint8_t *payload)
{
    out[0] = op;
    out[1] = 0;
    out[2] = (uint8_t)tag;
    out[3] = (uint8_t)(tag >> 8);
    put_le32(out + 4, lba);
    put_le32(out + 8, count);
    put_le32(out + 12, 0);
    if (payload != NULL) {
        for (uint32_t at = 0; at < count * YAN_OS_BLOCK_BLOCK_SIZE; ++at) {
            out[YAN_OS_BLOCK_HEADER_SIZE + at] = payload[at];
        }
    }
}

/* ------------------------------------------------------------- take helpers */

static void arm_outputs(YanOsBlockResponse *response)
{
    response->op = 0xa1;
    response->status = 0xa2;
    response->tag = 0xa3a4;
    response->lba = 0xa5a6a7a8;
    response->count = 0xa9aaabac;
    read_buffer[0] = 0x5c;
    read_buffer[YAN_OS_BLOCK_BLOCK_SIZE / 2] = 0x5c;
    read_buffer[YAN_OS_BLOCK_BLOCK_SIZE - 1] = 0x5c;
}

static int outputs_untouched(const YanOsBlockResponse *response)
{
    return response->op == 0xa1 && response->status == 0xa2 &&
           response->tag == 0xa3a4 && response->lba == 0xa5a6a7a8 &&
           response->count == 0xa9aaabac && read_buffer[0] == 0x5c &&
           read_buffer[YAN_OS_BLOCK_BLOCK_SIZE / 2] == 0x5c &&
           read_buffer[YAN_OS_BLOCK_BLOCK_SIZE - 1] == 0x5c;
}

/* Takes one response, remembering what a partial frame looked like while the
 * layer said AGAIN. Returns the take() result; on 0 the response is filled. */
static int take_watching(YanOsBlockResponse *response, uint32_t *length,
                         uint32_t tail_start, uint32_t budget)
{
    for (uint32_t spins = 0; spins < budget; ++spins) {
        ++yan_block_check_polls;
        const int result =
            yan_os_block_take(response, read_buffer, sizeof read_buffer, length);
        if (result == 0) {
            return 0;
        }
        if (result != YAN_OS_BLOCK_AGAIN) {
            return result;
        }
        const uint32_t available = yan_os_transport_h2g_available();
        if (available > 0 && available < YAN_OS_BLOCK_HEADER_SIZE) {
            yan_block_check_partial_seen = 1;
        }
        if (available >= YAN_OS_BLOCK_HEADER_SIZE) {
            yan_block_check_partial_payload_seen = 1;
        }
        if (yan_os_transport_h2g_tail() != tail_start) {
            yan_block_check_tail_moved = 1;
        }
        if (!outputs_untouched(response)) {
            yan_block_check_output_touched = 1;
        }
    }
    return YAN_OS_BLOCK_AGAIN;
}

/* Takes one response assuming it is already complete, with the same evidence
 * bookkeeping as take_watching but no budget. */
static int take_plain(YanOsBlockResponse *response, uint32_t *length)
{
    for (uint32_t spins = 0; spins < 500U; ++spins) {
        ++yan_block_check_polls;
        const int result =
            yan_os_block_take(response, read_buffer, sizeof read_buffer, length);
        if (result == 0) {
            return 0;
        }
        if (result != YAN_OS_BLOCK_AGAIN) {
            return result;
        }
        if (!outputs_untouched(response)) {
            yan_block_check_output_touched = 1;
        }
    }
    return YAN_OS_BLOCK_AGAIN;
}

/* --------------------------------------------------------------- scenarios */

static int submit_read(uint16_t tag, uint32_t lba, uint32_t count)
{
    const YanOsBlockRequest request = {
        .op = YAN_OS_BLOCK_OP_READ, .flags = 0, .tag = tag, .lba = lba, .count = count};
    return yan_os_block_submit(&request, NULL);
}

static int submit_write(uint16_t tag, uint32_t lba, uint32_t count,
                        const uint8_t *data)
{
    const YanOsBlockRequest request = {.op = YAN_OS_BLOCK_OP_WRITE, .flags = 0,
                                       .tag = tag, .lba = lba, .count = count};
    return yan_os_block_submit(&request, data);
}

static int submit_capacity(uint16_t tag)
{
    const YanOsBlockRequest request = {.op = YAN_OS_BLOCK_OP_CAPACITY,
                                       .flags = 0, .tag = tag, .lba = 0, .count = 0};
    return yan_os_block_submit(&request, NULL);
}

/* The capacity query and the completion notification that announces it. */
static inline void scenario_capacity(void)
{
    expect_u32(yan_os_transport_ring_size(), YAN_BLOCK_EXPECT_RING_SIZE,
               "the channel geometry");

    /* The channel's line reaches the CPU through PLIC source 1. Enabling the
     * source without enabling interrupts in the CPU keeps this a polling check
     * of the notification path: the line and the gateway, no handler. */
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_set_threshold(0);
    yan_os_transport_set_irq_enable(1);
    expect_u32(yan_os_transport_irq_status(), 0, "no interrupt before a response");

    expect_u32(submit_capacity(0x0101), 0, "the query is submitted");

    uint32_t spins = 0;
    while ((yan_os_transport_irq_status() & YAN_OS_TRANSPORT_IRQ_H2G_DATA) == 0 &&
           spins < 2000U) {
        ++spins;
    }
    ++yan_block_check_polls;
    expect(yan_os_transport_irq_status() != 0,
           "the host notifies only once the response is complete");
    expect(yan_os_transport_h2g_available() >= YAN_OS_BLOCK_HEADER_SIZE + 8U,
           "the whole response is in the ring when the line is asserted");

    /* The platform samples the device line before every instruction, so a few
     * instructions are what it takes for the PLIC to see it. */
    for (volatile uint32_t spin = 0; spin < 8U; ++spin) {
    }
    expect_u32(yan_os_plic_claim(), YAN_OS_PLIC_SOURCE_TRANSPORT,
               "the channel rides PLIC source 1");
    yan_os_transport_irq_ack();
    expect_u32(yan_os_transport_irq_status(), 0, "acking clears the channel status");
    yan_os_plic_complete(YAN_OS_PLIC_SOURCE_TRANSPORT);
    for (volatile uint32_t spin = 0; spin < 8U; ++spin) {
    }
    expect_u32(yan_os_plic_claim(), 0, "a withdrawn source does not come back");

    YanOsBlockResponse response;
    uint32_t length = 0;
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the response is delivered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_CAPACITY, "op echoed");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "status ok");
    expect_u32(response.tag, 0x0101, "tag echoed");
    expect_u32(response.lba, 0, "lba echoed");
    expect_u32(response.count, 0, "a query completes no blocks");
    expect_u32(length, 8, "the capacity is eight bytes");
    expect_u32((uint32_t)yan_os_block_count_from_bytes(read_buffer),
               YAN_BLOCK_EXPECT_BLOCKS, "the capacity is the attached device");
    expect_u32(yan_os_transport_h2g_available(), 0, "the ring is drained");
}

/* Whole responses, one publish each: the ordinary path. The scenario writes
 * the content it later checks, so it does not depend on what the backing store
 * held before the run - which is what lets the same image run under
 * `yan_run --disk`, whose store starts zeroed. */
static inline void scenario_read(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    expect_u32(yan_os_transport_h2g_available(), 0, "the ring starts empty");

    /* Round one: block 2. */
    fill_written(write_buffer, 2);
    expect_u32(submit_write(0x0201, 2, 1, write_buffer), 0, "the first write is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the first write is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the first write succeeded");

    expect_u32(submit_read(0x0202, 2, 1), 0, "the read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the read is answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_READ, "op echoed");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "status ok");
    expect_u32(response.tag, 0x0202, "tag echoed");
    expect_u32(response.lba, 2, "lba echoed");
    expect_u32(response.count, 1, "one block completed");
    expect_u32(length, YAN_OS_BLOCK_BLOCK_SIZE, "one block of payload");
    expect(matches_written(read_buffer, 2), "the payload is block 2");
    expect_u32(yan_os_transport_h2g_available(), 0, "exactly one frame arrived");

    /* Round two: another block, to show the layer is not a one-shot and that
     * the block number really selects the block. */
    fill_written(write_buffer, 5);
    expect_u32(submit_write(0x0203, 5, 1, write_buffer), 0, "the second write is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the second write is answered");
    expect_u32(response.tag, 0x0203, "the second write's tag");

    expect_u32(submit_read(0x0204, 5, 1), 0, "the second read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the second read is answered");
    expect_u32(response.tag, 0x0204, "the second tag is echoed");
    expect_u32(response.lba, 5, "the second lba is echoed");
    expect(matches_written(read_buffer, 5), "the payload is block 5");

    /* The last block of the device is inside it. */
    expect_u32(submit_read(0x0205, YAN_BLOCK_EXPECT_BLOCKS - 1, 1), 0,
               "the last block is requested");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the last block is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the last block is served");
    expect_u32(response.lba, YAN_BLOCK_EXPECT_BLOCKS - 1, "the last lba is echoed");
    expect_u32(length, YAN_OS_BLOCK_BLOCK_SIZE, "the last block has a payload");

    /* And a query on the same connection. */
    expect_u32(submit_capacity(0x0206), 0, "a query is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the query is answered");
    expect_u32((uint32_t)yan_os_block_count_from_bytes(read_buffer),
               YAN_BLOCK_EXPECT_BLOCKS, "the capacity is unchanged");
}

/* A write and the read that proves it landed. */
static inline void scenario_write(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    fill_written(write_buffer, 3);
    expect_u32(submit_write(0x0301, 3, 1, write_buffer), 0, "the write is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the write is answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_WRITE, "op echoed");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the write was accepted");
    expect_u32(response.lba, 3, "the written lba is echoed");
    expect_u32(length, 0, "a write response carries no payload");

    expect_u32(submit_read(0x0302, 3, 1), 0, "the read-back is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the read-back is answered");
    expect_u32(length, YAN_OS_BLOCK_BLOCK_SIZE, "the read-back carries a block");
    int same = 1;
    for (uint32_t at = 0; at < sizeof write_buffer; ++at) {
        if (read_buffer[at] != write_buffer[at]) {
            same = 0;
            break;
        }
    }
    expect(same, "the block read back is the block written");
}

/* Every parameter error the spec names, each one followed by a query so a
 * mis-sized consumption cannot hide behind the next request. */
static inline void scenario_params(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    struct {
        uint8_t op;
        uint32_t lba;
        uint32_t count;
        const char *name;
    } cases[] = {
        {YAN_OS_BLOCK_OP_READ, 0, 0, "a read of zero blocks is refused"},
        {YAN_OS_BLOCK_OP_READ, YAN_BLOCK_EXPECT_BLOCKS, 1,
         "a read past the end is refused"},
        {YAN_OS_BLOCK_OP_READ, UINT32_MAX, 1, "lba + count that wraps is refused"},
        {YAN_OS_BLOCK_OP_WRITE, YAN_BLOCK_EXPECT_BLOCKS, 1,
         "a write past the end is refused"},
        {YAN_OS_BLOCK_OP_CAPACITY, 1, 0, "a query with lba != 0 is refused"},
        {YAN_OS_BLOCK_OP_CAPACITY, 0, 1, "a query with count != 0 is refused"},
    };

    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const YanOsBlockRequest request = {.op = cases[i].op,
                                           .flags = 0,
                                           .tag = (uint16_t)(0x0400 + i),
                                           .lba = cases[i].lba,
                                           .count = cases[i].count};
        const uint8_t *payload = cases[i].op == YAN_OS_BLOCK_OP_WRITE ? write_buffer
                                                                     : NULL;
        expect_u32(yan_os_block_submit(&request, payload), 0, cases[i].name);

        arm_outputs(&response);
        expect_u32(take_plain(&response, &length), 0, "the refusal is delivered");
        expect_u32(response.op, cases[i].op, "the refused op is echoed");
        expect_u32(response.status, YAN_OS_BLOCK_STATUS_INVALID, cases[i].name);
        expect_u32(response.lba, cases[i].lba, "the refused lba is echoed");
        expect_u32(response.count, 0, "a refusal completes no blocks");
        expect_u32(length, 0, "a refusal carries no payload");

        expect_u32(submit_capacity((uint16_t)(0x0480 + i)), 0,
                   "the stream is still aligned");
        arm_outputs(&response);
        expect_u32(take_plain(&response, &length), 0, "the following query is answered");
        expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the following query succeeds");
        expect_u32((uint32_t)yan_os_block_count_from_bytes(read_buffer),
                   YAN_BLOCK_EXPECT_BLOCKS, "the following query is correct");
    }
}

/* Responses in request order, with the tag echoed: the property a caller
 * relies on instead of matching tags itself. */
static inline void scenario_order(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    expect_u32(submit_capacity(0x11), 0, "query submitted");
    expect_u32(take_plain(&response, &length), 0, "query answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_CAPACITY, "first response is the query");
    expect_u32(response.tag, 0x11, "first tag");

    fill_written(write_buffer, 2);
    expect_u32(submit_write(0x22, 2, 1, write_buffer), 0, "the first write is submitted");
    expect_u32(take_plain(&response, &length), 0, "the first write is answered");
    expect_u32(response.tag, 0x22, "second tag");

    expect_u32(submit_read(0x23, 2, 1), 0, "read submitted");
    expect_u32(take_plain(&response, &length), 0, "read answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_READ, "third response is the read");
    expect_u32(response.tag, 0x23, "third tag");
    expect(matches_written(read_buffer, 2), "third response carries block 2");

    fill_written(write_buffer, 6);
    expect_u32(submit_write(0x33, 6, 1, write_buffer), 0, "write submitted");
    expect_u32(take_plain(&response, &length), 0, "write answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_WRITE, "fourth response is the write");
    expect_u32(response.tag, 0x33, "fourth tag");

    expect_u32(submit_read(0x44, 6, 1), 0, "read-back submitted");
    expect_u32(take_plain(&response, &length), 0, "read-back answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_READ, "fifth response is the read");
    expect_u32(response.tag, 0x44, "fifth tag");
    int same = 1;
    for (uint32_t at = 0; at < sizeof write_buffer; ++at) {
        if (read_buffer[at] != write_buffer[at]) {
            same = 0;
            break;
        }
    }
    expect(same, "the last response carries what was written");

    expect_u32(submit_capacity(0x55), 0, "the last query is submitted");
    expect_u32(take_plain(&response, &length), 0, "the last query is answered");
    expect_u32(response.tag, 0x55, "the last tag");
}

/* The device-fault path: explicit, header-only, and it must not touch the
 * store or invent zero-filled data. The driver injects the fault before the
 * requests it is told to, so the Guest knows which ones fail. */
static inline void scenario_fault(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    /* 1: a read whose backend fails. */
    expect_u32(submit_read(0x0010, 0, 1), 0, "the failing read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the failing read is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_DEVICE, "the read reports a fault");
    expect_u32(response.count, 0, "a failed read completes no blocks");
    expect_u32(length, 0, "a failed read carries no data");
    expect(read_buffer[0] == 0x5c && read_buffer[YAN_OS_BLOCK_BLOCK_SIZE / 2] == 0x5c &&
               read_buffer[YAN_OS_BLOCK_BLOCK_SIZE - 1] == 0x5c,
           "a failed read does not fill the caller's buffer");

    /* 2: the same read now works, so the fault was one-shot. */
    expect_u32(submit_read(0x0011, 0, 1), 0, "the next read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the next read is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the fault was one-shot");
    expect(matches_pattern(read_buffer, 0), "the retry returns the block");

    /* 3: a query in between is answered normally. */
    expect_u32(submit_capacity(0x0012), 0, "the query is submitted");
    expect_u32(take_plain(&response, &length), 0, "the query is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "a query succeeds");

    /* 4: a write whose backend fails. */
    fill_written(write_buffer, 2);
    expect_u32(submit_write(0x0013, 2, 1, write_buffer), 0, "the failing write is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the failing write is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_DEVICE, "the write reports a fault");
    expect_u32(length, 0, "a failed write carries no data");

    /* 5: the block still holds its old content: all or nothing. */
    expect_u32(submit_read(0x0014, 2, 1), 0, "the read-back is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the read-back is answered");
    expect(matches_pattern(read_buffer, 2), "the failed write left the block alone");

    /* 6: a query does not touch storage, so it does not consume the fault. */
    expect_u32(submit_capacity(0x0015), 0, "the query before the fault is submitted");
    expect_u32(take_plain(&response, &length), 0, "the query before the fault is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "a query is not a storage access");

    /* 7: the fault the query did not consume lands on the next read. */
    expect_u32(submit_read(0x0016, 1, 1), 0, "the read after the query is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the read after the query is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_DEVICE,
               "a query does not clear the pending fault");

    /* 8: and then the channel is healthy again. */
    expect_u32(submit_read(0x0017, 1, 1), 0, "the last read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the last read is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the channel recovered");
    expect(matches_pattern(read_buffer, 1), "the last read returns its block");
}

/* Space that is not there: the frame must stay out of the ring, all of it. The
 * driver serves nothing in this scenario, so the ring state is exactly what the
 * Guest left. */
static inline void scenario_flow_short(void)
{
    fill_written(write_buffer, 4);
    build_frame(frame_buffer, YAN_OS_BLOCK_OP_WRITE, 0x0077, 4, 1, write_buffer);
    const uint32_t frame_bytes = YAN_OS_BLOCK_HEADER_SIZE + YAN_OS_BLOCK_BLOCK_SIZE;

    expect_u32(yan_os_transport_g2h_space(), YAN_BLOCK_EXPECT_RING_SIZE - 1U,
               "an empty ring offers RING_SIZE - 1 bytes");
    expect_u32(yan_os_transport_g2h_push(frame_buffer, frame_bytes), 0,
               "a full frame fits an empty ring");
    expect_u32(yan_os_transport_g2h_head(), frame_bytes, "the frame is in the ring");
    expect_u32(yan_os_transport_g2h_space(), YAN_BLOCK_EXPECT_RING_SIZE - 1U - frame_bytes,
               "the ring has only the remainder free");

    /* One block needs 4112 bytes, of which 4079 are left. */
    expect_u32(submit_write(0x0078, 5, 1, write_buffer), YAN_OS_BLOCK_AGAIN,
               "a frame that does not fit is refused");
    expect_u32(yan_os_transport_g2h_head(), frame_bytes,
               "a refused frame writes no byte at all");
    expect_u32(yan_os_transport_g2h_space(), YAN_BLOCK_EXPECT_RING_SIZE - 1U - frame_bytes,
               "a refused frame leaves the space untouched");
    expect_u32(submit_write(0x0079, 5, 1, write_buffer), YAN_OS_BLOCK_AGAIN,
               "the refusal repeats without half a frame accumulating");
    expect_u32(yan_os_transport_g2h_head(), frame_bytes, "and still writes nothing");
    expect_u32(yan_os_transport_h2g_available(), 0, "and provokes no response");
}

/* Yielding until the space is back, then the same request succeeds. */
static inline void scenario_flow_retry(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;
    fill_written(write_buffer, 4);
    build_frame(frame_buffer, YAN_OS_BLOCK_OP_WRITE, 0x0077, 4, 1, write_buffer);
    const uint32_t frame_bytes = YAN_OS_BLOCK_HEADER_SIZE + YAN_OS_BLOCK_BLOCK_SIZE;

    expect_u32(yan_os_transport_g2h_push(frame_buffer, frame_bytes), 0,
               "a full frame fits an empty ring");
    fill_written(write_buffer, 5);
    expect_u32(submit_write(0x0078, 5, 1, write_buffer), YAN_OS_BLOCK_AGAIN,
               "the second frame does not fit yet");
    expect_u32(yan_os_transport_g2h_head(), frame_bytes, "and wrote nothing");

    /* The queued frame is only a request once the bell rings. */
    yan_os_transport_doorbell();
    uint32_t spins = 0;
    while (yan_os_transport_g2h_space() < frame_bytes && spins < 2000U) {
        ++spins;
    }
    expect_u32(yan_os_transport_g2h_space(), YAN_BLOCK_EXPECT_RING_SIZE - 1U,
               "the host consumed the queued frame and freed the ring");

    expect_u32(submit_write(0x0078, 5, 1, write_buffer), 0, "the retry is accepted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the queued frame is answered");
    expect_u32(response.op, YAN_OS_BLOCK_OP_WRITE, "the queued frame was a write");
    expect_u32(response.tag, 0x0077, "the queued frame's tag");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the queued write succeeded");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the retried frame is answered");
    expect_u32(response.tag, 0x0078, "the retried frame's tag");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "the retry succeeded");
    expect_u32(yan_os_transport_h2g_available(), 0, "both responses were taken in order");
}

/* What the layer refuses to build at all: it never puts half a header in the
 * ring for a request that could not be sent. */
static inline void scenario_limit(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    /* RING_SIZE 8192 leaves 8191 bytes; 16 + 4096 fits once, 16 + 8192 does
     * not, so one block is the whole of v1 on this channel. */
    expect_u32(yan_os_block_max_count(), 1, "one block is the largest request");
    expect_u32(yan_os_transport_g2h_head(), 0, "the ring starts empty");

    expect_u32(submit_read(0x0501, 0, 2), YAN_OS_BLOCK_INVALID,
               "a two-block read is refused before anything is written");
    expect_u32(submit_write(0x0502, 0, 2, write_buffer), YAN_OS_BLOCK_INVALID,
               "a two-block write is refused");
    expect_u32(submit_read(0x0503, 0, UINT32_C(0x00100001)), YAN_OS_BLOCK_INVALID,
               "a count whose size wraps is refused");
    expect_u32(submit_read(0x0504, 0, UINT32_C(0xffffffff)), YAN_OS_BLOCK_INVALID,
               "the largest count is refused");
    expect_u32(yan_os_transport_g2h_head(), 0, "nothing was written by any refusal");
    expect_u32(yan_os_transport_g2h_space(), YAN_BLOCK_EXPECT_RING_SIZE - 1U,
               "the ring is untouched");

    const YanOsBlockRequest unknown = {.op = 9, .flags = 0, .tag = 0x0505, .lba = 0, .count = 0};
    expect_u32(yan_os_block_submit(&unknown, NULL), YAN_OS_BLOCK_INVALID,
               "an unknown op is refused");
    const YanOsBlockRequest flagged = {.op = YAN_OS_BLOCK_OP_READ, .flags = 1,
                                       .tag = 0x0506, .lba = 0, .count = 1};
    expect_u32(yan_os_block_submit(&flagged, NULL), YAN_OS_BLOCK_INVALID,
               "a request with flags set is refused");
    const YanOsBlockRequest write_without_data = {.op = YAN_OS_BLOCK_OP_WRITE, .flags = 0,
                                                  .tag = 0x0509, .lba = 0, .count = 1};
    expect_u32(yan_os_block_submit(&write_without_data, NULL), YAN_OS_BLOCK_INVALID,
               "a write without data is refused");
    /* A query whose lba or count is not zero is *not* refused here: it is a
     * legal frame and the device answers status 1, which scenario_params
     * checks. This scenario is about what cannot be a frame at all. */
    expect_u32(yan_os_transport_g2h_head(), 0, "still nothing was written");

    /* The largest frame the channel can carry is one block, and it works. The
     * bytes are the pattern inverted, so the drive can tell a write that landed
     * from a block that was already there. */
    fill_written(write_buffer, 0);
    expect_u32(submit_write(0x050a, 0, 1, write_buffer), 0,
               "the largest sendable frame is accepted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "it is answered");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "it succeeds");
    expect_u32(response.tag, 0x050a, "its tag is echoed");
}

/* A response published in pieces. The driver paces the pieces, so the Guest
 * really does observe the incomplete states; what it must do about them is
 * nothing at all except wait. */
static inline void scenario_split(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    const uint32_t tail_start = yan_os_transport_h2g_tail();
    expect_u32(submit_read(0x1234, 1, 1), 0, "the read is submitted");

    arm_outputs(&response);
    const int result = take_watching(&response, &length, tail_start, 2000U);
    expect_u32(result, 0, "the response is delivered once it is complete");
    expect_u32(response.op, YAN_OS_BLOCK_OP_READ, "op echoed");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "status ok");
    expect_u32(response.tag, 0x1234, "tag echoed");
    expect_u32(response.lba, 1, "lba echoed");
    expect_u32(response.count, 1, "one block completed");
    expect_u32(length, YAN_OS_BLOCK_BLOCK_SIZE, "one block of payload");
    expect(matches_pattern(read_buffer, 1), "the payload survived the split arrival");

    const uint32_t size = yan_os_transport_ring_size();
    const uint32_t mask = size - 1U;
    const uint32_t frame_bytes = YAN_OS_BLOCK_HEADER_SIZE + YAN_OS_BLOCK_BLOCK_SIZE;
    const uint32_t tail_end = yan_os_transport_h2g_tail();
    expect_u32((tail_end - tail_start) & mask, frame_bytes,
               "the whole frame was consumed, once");
    yan_block_check_frame_wrapped =
        ((tail_start & mask) + frame_bytes > size) ? 1U : 0U;
    expect(yan_block_check_tail_moved == 0,
           "the tail never moved while the frame was incomplete");
    expect(yan_block_check_output_touched == 0,
           "no half frame was handed to the caller");
    expect(yan_block_check_partial_seen != 0 || yan_block_check_partial_payload_seen != 0,
           "an incomplete frame was really observed");
}

/* A frame that breaks the header rules is consumed by its length and dropped:
 * the next response must still parse. The driver injects the frame. */
static inline void scenario_malformed(void)
{
    YanOsBlockResponse response;
    uint32_t length = 0;

    const uint32_t tail_start = yan_os_transport_h2g_tail();
    expect_u32(submit_read(0x6001, 1, 1), 0, "the first read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), YAN_OS_BLOCK_MALFORMED,
               "the injected frame is reported as malformed");
    expect(outputs_untouched(&response), "a dropped frame writes nothing through outputs");
    /* The driver checks the exact number of bytes consumed; here it is enough
     * that something was consumed, because the frame length is the driver's
     * choice. */
    expect(yan_os_transport_h2g_tail() != tail_start, "the malformed frame was consumed");

    expect_u32(submit_read(0x6002, 1, 1), 0, "the second read is submitted");
    arm_outputs(&response);
    expect_u32(take_plain(&response, &length), 0, "the next response is still readable");
    expect_u32(response.op, YAN_OS_BLOCK_OP_READ, "op echoed after the dropped frame");
    expect_u32(response.status, YAN_OS_BLOCK_STATUS_OK, "status ok after the dropped frame");
    expect_u32(response.tag, 0x6002, "the next tag is echoed");
    expect(matches_pattern(read_buffer, 1), "the next payload is intact");
    expect_u32(yan_os_transport_h2g_available(), 0, "nothing is left behind");
}

/* Without a disk the channel is mapped but unconfigured. The layer must refuse
 * to touch the rings rather than run mask arithmetic on a zero size. */
static inline void scenario_no_host(void)
{
    expect_u32(yan_os_transport_host_ready(), 0, "no host is attached");
    expect_u32(yan_os_block_max_count(), 0, "no request fits an unconfigured channel");
    expect_u32(submit_read(0x0701, 0, 1), YAN_OS_BLOCK_AGAIN,
               "a submit without a channel is retryable, not fatal");
    YanOsBlockResponse response;
    uint32_t length = 0;
    arm_outputs(&response);
    expect_u32(yan_os_block_take(&response, read_buffer, sizeof read_buffer, &length),
               YAN_OS_BLOCK_INVALID, "take without a channel is rejected");
    expect(outputs_untouched(&response), "and writes nothing through its outputs");

    /* The arguments are still checked before the channel is: that order keeps a
     * caller bug from looking like a missing host. */
    expect_u32(yan_os_block_take(NULL, read_buffer, sizeof read_buffer, &length),
               YAN_OS_BLOCK_INVALID, "a NULL response is rejected");
    expect_u32(yan_os_block_submit(NULL, NULL), YAN_OS_BLOCK_INVALID,
               "a NULL request is rejected");
}

int main(void)
{
#if YAN_BLOCK_SCENARIO == SCENARIO_CAPACITY
    scenario_capacity();
#elif YAN_BLOCK_SCENARIO == SCENARIO_READ
    scenario_read();
#elif YAN_BLOCK_SCENARIO == SCENARIO_WRITE
    scenario_write();
#elif YAN_BLOCK_SCENARIO == SCENARIO_PARAMS
    scenario_params();
#elif YAN_BLOCK_SCENARIO == SCENARIO_ORDER
    scenario_order();
#elif YAN_BLOCK_SCENARIO == SCENARIO_FAULT
    scenario_fault();
#elif YAN_BLOCK_SCENARIO == SCENARIO_FLOW_SHORT
    scenario_flow_short();
#elif YAN_BLOCK_SCENARIO == SCENARIO_FLOW_RETRY
    scenario_flow_retry();
#elif YAN_BLOCK_SCENARIO == SCENARIO_LIMIT
    scenario_limit();
#elif YAN_BLOCK_SCENARIO == SCENARIO_SPLIT
    scenario_split();
#elif YAN_BLOCK_SCENARIO == SCENARIO_MALFORMED
    scenario_malformed();
#elif YAN_BLOCK_SCENARIO == SCENARIO_NO_HOST
    scenario_no_host();
#else
#error "unknown YAN_BLOCK_SCENARIO"
#endif
    emit_text(":PASS: ");
    emit_text(YAN_BLOCK_SCENARIO_NAME);
    emit_text("\n");
    guest_finish(1);
    return 0; /* not reached: guest_finish reports the verdict and stops */
}
