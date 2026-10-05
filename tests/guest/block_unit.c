/* Host-side unit checks for tools/host_block.c: the request pump and the codec
 * of docs/specs/0018-block-protocol.md.
 *
 * Why this file exists: the Guest drives of this suite prove that a Guest and a
 * Host agree, but they can only produce the frames a well-behaved Guest would
 * produce, and they cannot put the ring into a chosen state. Every rule the
 * spec states about *receiving* - fewer than 16 bytes are never interpreted, a
 * frame is consumed by its full length even when it fails validation, a count
 * that cannot fit the ring must not be awaited forever, a failed request is
 * answered with a header and nothing else - is therefore checked here, by
 * writing request bytes into the guest-to-host ring by hand and inspecting the
 * response bytes, the ring positions, the interrupt status and the backing
 * store.
 *
 * The channel, the RAM and the block device are the production objects; only
 * the Guest's stores into the ring are simulated, and they are simulated the
 * way a Guest performs them (bytes in RAM, then the position register).
 *
 * The storage pattern is restated in tests/guest/block_check.c. A drift between
 * the two shows up as a failed read check in both drives instead of cancelling
 * out.
 *
 * A failing check prints Unity's ":FAIL:" marker so that the mutation script
 * can tell a detection (an assertion) apart from a crash, a hang or a
 * sanitizer report, none of which count. Exit codes: 0 every check passed, 1 at
 * least one check failed, 2 the fixture could not be built. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_block.h"
#include "yan/ram.h"
#include "yan/transport.h"

#define FIXTURE_BASE UINT32_C(0x80000000)
#define FIXTURE_RAM (UINT32_C(256) * 1024U)
#define FIXTURE_RING UINT32_C(8192)
#define FIXTURE_BLOCKS UINT64_C(8)
#define HEADER_SIZE UINT32_C(16)

static unsigned checks;
static unsigned failures;

static void report(int ok, const char *name)
{
    ++checks;
    if (ok) {
        printf("ok   %s\n", name);
        return;
    }
    printf(":FAIL: %s\n", name);
    ++failures;
}

static void report_u64(uint64_t actual, uint64_t expected, const char *name)
{
    ++checks;
    if (actual == expected) {
        printf("ok   %s\n", name);
        return;
    }
    printf(":FAIL: %s: expected %" PRIu64 ", got %" PRIu64 "\n", name, expected,
           actual);
    ++failures;
}

#define CHECK(condition, name) report((condition) ? 1 : 0, (name))
#define CHECK_U64(actual, expected, name) report_u64((actual), (expected), (name))
#define SECTION(name) printf("# %s\n", (name))

/* One byte of block `lba` at `offset`. Varies with both so a copy that loses
 * the block number, the offset or a whole 4096-byte chunk is visible. */
static uint8_t pattern(uint32_t lba, uint32_t offset)
{
    return (uint8_t)((lba * 37U + offset * 11U + (offset >> 8) * 5U) & 0xffU);
}

typedef struct {
    YanRam ram;
    YanTransport transport;
    YanHostBlock block;
    uint8_t *storage;
    uint32_t ring_base;
    /* The Guest's production position. The device keeps it, but the Guest is
     * the writer here, so the fixture mirrors it the way a Guest would. */
    uint32_t g2h_head;
} Fixture;

static void notify(void *context)
{
    (void)context;
}

static void fixture_init(Fixture *fixture, uint64_t blocks)
{
    memset(fixture, 0, sizeof *fixture);
    if (yan_ram_init(&fixture->ram, FIXTURE_RAM) != YAN_OK) {
        fprintf(stderr, "block_unit: cannot allocate the fixture RAM\n");
        exit(2);
    }
    fixture->ring_base = FIXTURE_BASE + FIXTURE_RAM - 2U * FIXTURE_RING;
    if (yan_transport_configure(&fixture->transport, fixture->ring_base,
                                FIXTURE_RING, FIXTURE_BASE,
                                FIXTURE_RAM) != YAN_OK) {
        fprintf(stderr, "block_unit: cannot place the rings\n");
        exit(2);
    }
    /* Registering a callback is what makes HOST_READY true; the pump is the
     * caller of this fixture, so the callback itself never runs. */
    yan_transport_set_notify(&fixture->transport, notify, NULL);
    if (blocks != 0) {
        fixture->storage = calloc((size_t)blocks, YAN_HOST_BLOCK_BLOCK_SIZE);
        if (fixture->storage == NULL) {
            fprintf(stderr, "block_unit: cannot allocate the backing store\n");
            exit(2);
        }
        for (uint32_t lba = 0; lba < blocks; ++lba) {
            for (uint32_t at = 0; at < YAN_HOST_BLOCK_BLOCK_SIZE; ++at) {
                fixture->storage[(size_t)lba * YAN_HOST_BLOCK_BLOCK_SIZE + at] =
                    pattern(lba, at);
            }
        }
    }
    if (yan_host_block_init(&fixture->block, fixture->storage, blocks) != YAN_OK) {
        fprintf(stderr, "block_unit: cannot initialise the block device\n");
        exit(2);
    }
}

static void fixture_done(Fixture *fixture)
{
    free(fixture->storage);
    yan_ram_destroy(&fixture->ram);
}

/* -------------------------------------------------------------- ring access */

static uint8_t *ring_bytes(Fixture *fixture, uint32_t ring)
{
    /* The two rings are one region: the host-to-guest ring starts where the
     * guest-to-host ring ends (0014). `ring` is 0 for guest-to-host. */
    return fixture->ram.data + (fixture->ring_base - FIXTURE_BASE) +
           ring * FIXTURE_RING;
}

/* The Guest's side of the guest-to-host ring: bytes first, position second. */
static void guest_push(Fixture *fixture, const uint8_t *bytes, uint32_t count)
{
    uint8_t *ring = ring_bytes(fixture, 0);
    for (uint32_t i = 0; i < count; ++i) {
        ring[(fixture->g2h_head + i) & (FIXTURE_RING - 1U)] = bytes[i];
    }
    fixture->g2h_head = (fixture->g2h_head + count) & (FIXTURE_RING - 1U);
    if (yan_transport_write(&fixture->transport, YAN_TRANSPORT_G2H_HEAD,
                            fixture->g2h_head) != YAN_OK) {
        fprintf(stderr, "block_unit: cannot publish the guest head\n");
        exit(2);
    }
}

/* The Guest's consumption of the host-to-guest ring, which is the tail. */
static void guest_consume(Fixture *fixture, uint32_t count)
{
    uint32_t tail = 0;
    if (yan_transport_read(&fixture->transport, YAN_TRANSPORT_H2G_TAIL,
                           &tail) != YAN_OK) {
        fprintf(stderr, "block_unit: cannot read the guest tail\n");
        exit(2);
    }
    tail = (tail + count) & (FIXTURE_RING - 1U);
    if (yan_transport_write(&fixture->transport, YAN_TRANSPORT_H2G_TAIL,
                            tail) != YAN_OK) {
        fprintf(stderr, "block_unit: cannot advance the guest tail\n");
        exit(2);
    }
}

static uint32_t host_tail(const Fixture *fixture)
{
    return fixture->transport.g2h_tail;
}

static uint32_t irq_status(const Fixture *fixture)
{
    return fixture->transport.irq_status & YAN_TRANSPORT_IRQ_H2G_DATA;
}

/* Bytes the host has published but the Guest has not consumed. */
static uint32_t published(const Fixture *fixture)
{
    return (fixture->transport.h2g_head - fixture->transport.h2g_tail) &
           (FIXTURE_RING - 1U);
}

/* Reads `count` published bytes starting at the Guest's tail. */
static void published_bytes(const Fixture *fixture, uint32_t offset, uint8_t *out,
                            uint32_t count)
{
    const uint8_t *ring = fixture->ram.data + (fixture->ring_base - FIXTURE_BASE) +
                          FIXTURE_RING;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = ring[(fixture->transport.h2g_tail + offset + i) & (FIXTURE_RING - 1U)];
    }
}

static uint32_t serve(Fixture *fixture)
{
    return yan_host_block_service(&fixture->block, &fixture->transport,
                                  &fixture->ram, FIXTURE_BASE);
}

/* ---------------------------------------------------------------- requests */

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

/* The header a Guest would build, written field by field so the test does not
 * depend on the struct layout or on the host's own encoder. */
static void build_header(uint8_t out[16], uint8_t op, uint8_t flags, uint16_t tag,
                         uint32_t lba, uint32_t count, uint32_t reserved)
{
    memset(out, 0, 16);
    out[0] = op;
    out[1] = flags;
    put_le16(out + 2, tag);
    put_le32(out + 4, lba);
    put_le32(out + 8, count);
    put_le32(out + 12, reserved);
}

static void push_request(Fixture *fixture, uint8_t op, uint8_t flags, uint16_t tag,
                         uint32_t lba, uint32_t count, uint32_t reserved,
                         const uint8_t *payload)
{
    uint8_t header[16];
    build_header(header, op, flags, tag, lba, count, reserved);
    guest_push(fixture, header, 16);
    if (payload != NULL && count != 0) {
        guest_push(fixture, payload, count * YAN_HOST_BLOCK_BLOCK_SIZE);
    }
}

static void push_capacity_query(Fixture *fixture, uint16_t tag)
{
    push_request(fixture, 3, 0, tag, 0, 0, 0, NULL);
}

/* The response header at `offset` bytes into what the host published, decoded
 * field by field. */
typedef struct {
    uint8_t op;
    uint8_t status;
    uint16_t tag;
    uint32_t lba;
    uint32_t count;
    uint32_t reserved;
} Reply;

static Reply reply_at(const Fixture *fixture, uint32_t offset)
{
    uint8_t bytes[16];
    published_bytes(fixture, offset, bytes, 16);
    Reply reply = {0};
    reply.op = bytes[0];
    reply.status = bytes[1];
    reply.tag = (uint16_t)(bytes[2] | ((uint16_t)bytes[3] << 8));
    for (uint32_t i = 0; i < 4; ++i) {
        reply.lba |= (uint32_t)bytes[4 + i] << (8U * i);
        reply.count |= (uint32_t)bytes[8 + i] << (8U * i);
        reply.reserved |= (uint32_t)bytes[12 + i] << (8U * i);
    }
    return reply;
}

static uint64_t reply_capacity(const Fixture *fixture, uint32_t offset)
{
    uint8_t bytes[8];
    published_bytes(fixture, offset + 16, bytes, 8);
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    return value;
}

/* ------------------------------------------------------------------ checks */

/* 0018 "整帧原子性与部分到达" rule 1: fewer than 16 bytes are not interpreted,
 * and the bytes stay where they are until the header is complete. */
static void check_partial_header(void)
{
    SECTION("a partial header is not interpreted");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    uint8_t header[16];
    build_header(header, 3, 0, 0x4242, 0, 0, 0);

    for (uint32_t present = 1; present < 16; ++present) {
        guest_push(&fixture, header + present - 1, 1);
        CHECK_U64(serve(&fixture), 0, "no request answered before 16 bytes");
        CHECK_U64(host_tail(&fixture), 0, "nothing consumed before 16 bytes");
        CHECK_U64(published(&fixture), 0, "no response before 16 bytes");
        CHECK_U64(irq_status(&fixture), 0, "no interrupt before 16 bytes");
        CHECK(!fixture.transport.overflow_detected,
              "no overrun is latched before 16 bytes");
    }
    guest_push(&fixture, header + 15, 1);
    CHECK_U64(serve(&fixture), 1, "the complete header is answered");
    CHECK_U64(host_tail(&fixture), 16, "the request is consumed once complete");
    CHECK_U64(published(&fixture), 24, "the capacity reply is published");
    CHECK_U64(reply_at(&fixture, 0).status, 0, "the late header parsed correctly");

    fixture_done(&fixture);
}

/* The header arriving in two pieces, 8 + 8. */
static void check_header_split(void)
{
    SECTION("a header that arrives 8 + 8");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    uint8_t header[16];
    build_header(header, 3, 0, 0x0909, 0, 0, 0);
    guest_push(&fixture, header, 8);
    CHECK_U64(serve(&fixture), 0, "half a header answers nothing");
    CHECK_U64(host_tail(&fixture), 0, "half a header consumes nothing");
    guest_push(&fixture, header + 8, 8);
    CHECK_U64(serve(&fixture), 1, "the second half completes the request");
    const Reply reply = reply_at(&fixture, 0);
    CHECK_U64(reply.op, 3, "op echoed");
    CHECK_U64(reply.status, 0, "status ok");
    CHECK_U64(reply.tag, 0x0909, "tag echoed");
    CHECK_U64(reply_capacity(&fixture, 0), FIXTURE_BLOCKS, "capacity answered");

    fixture_done(&fixture);
}

/* Whole frames, both directions, one service call: the readable case. */
static void check_read_whole_frame(void)
{
    SECTION("a whole read request in one arrival");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    CHECK_U64(irq_status(&fixture), 0, "no interrupt before any request");
    push_request(&fixture, 1, 0, 0x1234, 2, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 1, "the read is answered");
    CHECK_U64(host_tail(&fixture), 16, "the 16-byte request is consumed");
    CHECK_U64(published(&fixture), 16 + 4096, "header and one block are published");
    const Reply reply = reply_at(&fixture, 0);
    CHECK_U64(reply.op, 1, "op echoed");
    CHECK_U64(reply.status, 0, "status ok");
    CHECK_U64(reply.tag, 0x1234, "tag echoed");
    CHECK_U64(reply.lba, 2, "lba echoed");
    CHECK_U64(reply.count, 1, "count reports the completed blocks");
    CHECK_U64(reply.reserved, 0, "reserved stays zero");
    uint8_t block[4096];
    published_bytes(&fixture, 16, block, sizeof block);
    int matches = 1;
    for (uint32_t at = 0; at < sizeof block; ++at) {
        if (block[at] != pattern(2, at)) {
            matches = 0;
            break;
        }
    }
    CHECK(matches, "the payload is block 2's content");
    CHECK_U64(irq_status(&fixture), 1, "the interrupt is asserted with the data");
    /* Acking withdraws the line without touching the data: the two are
     * separate actions (0014). */
    CHECK(yan_transport_write(&fixture.transport, YAN_TRANSPORT_IRQ_STATUS,
                              YAN_TRANSPORT_IRQ_H2G_DATA) == YAN_OK,
          "the interrupt status is writable");
    CHECK_U64(irq_status(&fixture), 0, "the cleared interrupt reads back clear");
    CHECK_U64(published(&fixture), 16 + 4096, "clearing does not consume the data");

    fixture_done(&fixture);
}

/* A write request followed by a capacity query, both complete in the ring
 * before the service runs. Consuming the write by its full length is what keeps
 * the query readable; a pump that consumed only the header would parse the
 * payload as the next header. */
static void check_write_then_query(void)
{
    SECTION("a write followed by a query, no misalignment");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    uint8_t payload[4096];
    for (uint32_t at = 0; at < sizeof payload; ++at) {
        payload[at] = (uint8_t)(at * 3U + 1U);
    }
    push_request(&fixture, 2, 0, 0x00a1, 3, 1, 0, payload);
    push_capacity_query(&fixture, 0x00a2);
    CHECK_U64(serve(&fixture), 2, "both requests are answered");
    CHECK_U64(host_tail(&fixture), 16 + 4096 + 16, "both frames are consumed");

    const Reply write_reply = reply_at(&fixture, 0);
    CHECK_U64(write_reply.op, 2, "write op echoed");
    CHECK_U64(write_reply.status, 0, "write accepted");
    CHECK_U64(write_reply.tag, 0x00a1, "write tag echoed");
    const Reply query_reply = reply_at(&fixture, 16);
    CHECK_U64(query_reply.op, 3, "the query follows at the right offset");
    CHECK_U64(query_reply.status, 0, "the query is not damaged by the payload");
    CHECK_U64(query_reply.tag, 0x00a2, "the query tag is intact");
    CHECK_U64(reply_capacity(&fixture, 16), FIXTURE_BLOCKS, "the query is answered");
    CHECK_U64(published(&fixture), 16 + 24, "only the two headers are published");

    int stored = 1;
    for (uint32_t at = 0; at < sizeof payload; ++at) {
        if (fixture.storage[3 * 4096 + at] != payload[at]) {
            stored = 0;
            break;
        }
    }
    CHECK(stored, "the written block reached the backing store");

    fixture_done(&fixture);
}

/* A rejected frame is consumed by its full length. Three shapes: non-zero
 * flags, non-zero reserved and an unknown op. Each is followed by a query that
 * can only be answered if the stream stayed aligned. */
static void check_invalid_frames_consumed(void)
{
    SECTION("rejected frames are consumed by their frame length");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    uint8_t payload[4096];
    memset(payload, 0x5a, sizeof payload);

    push_request(&fixture, 2, 1, 0x0101, 1, 1, 0, payload);
    push_capacity_query(&fixture, 0x0102);
    CHECK_U64(serve(&fixture), 2, "flags case: both frames answered");
    CHECK_U64(host_tail(&fixture), 16 + 4096 + 16, "flags case: full frame consumed");
    CHECK_U64(reply_at(&fixture, 0).status, 1, "flags case: parameter error");
    CHECK_U64(reply_at(&fixture, 0).count, 0, "flags case: no blocks claimed");
    CHECK_U64(published(&fixture), 16 + 24,
              "flags case: the error reply is a header only");
    CHECK_U64(reply_at(&fixture, 16).op, 3, "flags case: the next frame is a query");
    CHECK_U64(reply_capacity(&fixture, 16), FIXTURE_BLOCKS, "flags case: query answered");
    CHECK_U64(fixture.storage[1 * 4096], pattern(1, 0),
              "flags case: the rejected write did not reach the store");

    fixture_done(&fixture);

    fixture_init(&fixture, FIXTURE_BLOCKS);
    push_request(&fixture, 2, 0, 0x0201, 1, 1, 7, payload);
    push_capacity_query(&fixture, 0x0202);
    CHECK_U64(serve(&fixture), 2, "reserved case: both frames answered");
    CHECK_U64(host_tail(&fixture), 16 + 4096 + 16, "reserved case: full frame consumed");
    CHECK_U64(reply_at(&fixture, 0).status, 1, "reserved case: parameter error");
    CHECK_U64(published(&fixture), 16 + 24, "reserved case: header-only error reply");
    CHECK_U64(reply_at(&fixture, 16).op, 3, "reserved case: next frame is a query");
    CHECK_U64(fixture.storage[1 * 4096], pattern(1, 0),
              "reserved case: the rejected write did not reach the store");
    fixture_done(&fixture);

    fixture_init(&fixture, FIXTURE_BLOCKS);
    uint8_t unknown[16];
    build_header(unknown, 9, 0, 0x0301, 0, 0, 0);
    guest_push(&fixture, unknown, 16);
    push_capacity_query(&fixture, 0x0302);
    CHECK_U64(serve(&fixture), 2, "unknown op case: both frames answered");
    CHECK_U64(host_tail(&fixture), 32, "unknown op case: one header consumed each");
    CHECK_U64(reply_at(&fixture, 0).status, 3, "unknown op case: unsupported");
    CHECK_U64(reply_at(&fixture, 0).op, 9, "unknown op case: op echoed back");
    CHECK_U64(reply_at(&fixture, 0).count, 0, "unknown op case: no blocks claimed");
    CHECK_U64(reply_at(&fixture, 16).op, 3, "unknown op case: next frame is a query");
    CHECK_U64(reply_capacity(&fixture, 16), FIXTURE_BLOCKS,
              "unknown op case: query answered");
    fixture_done(&fixture);
}

/* Every documented parameter error, each followed by a query so a mis-sized
 * consumption cannot hide. */
static void check_parameter_errors(void)
{
    SECTION("parameter errors are explicit");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    struct {
        uint8_t op;
        uint32_t lba;
        uint32_t count;
        const char *name;
    } cases[] = {
        {1, 0, 0, "count == 0"},
        {1, (uint32_t)FIXTURE_BLOCKS, 1, "lba at the end of the device"},
        {1, UINT32_MAX, 1, "lba + count wraps 32 bits"},
        {2, (uint32_t)FIXTURE_BLOCKS, 1, "a write past the end"},
        {3, 1, 0, "capacity query with lba != 0"},
        {3, 0, 1, "capacity query with count != 0"},
    };
    const uint32_t case_count = (uint32_t)(sizeof cases / sizeof cases[0]);

    uint32_t offset = 0;
    uint32_t consumed = 0;
    for (uint32_t i = 0; i < case_count; ++i) {
        /* A rejected write still carries its payload: the stream has it, so it
         * has to be consumed with the header. */
        const uint32_t payload_blocks = cases[i].op == 2 ? cases[i].count : 0;
        const uint32_t frame_bytes = 16 + payload_blocks * YAN_HOST_BLOCK_BLOCK_SIZE;
        push_request(&fixture, cases[i].op, 0, (uint16_t)(0x100 + i), cases[i].lba,
                     cases[i].count, 0, NULL);
        if (payload_blocks != 0) {
            uint8_t block[4096];
            memset(block, 0x11, sizeof block);
            guest_push(&fixture, block, payload_blocks * YAN_HOST_BLOCK_BLOCK_SIZE);
        }
        push_capacity_query(&fixture, (uint16_t)(0x200 + i));

        CHECK_U64(serve(&fixture), 2, cases[i].name);
        consumed += frame_bytes + 16;
        CHECK_U64(host_tail(&fixture), consumed,
                  "the rejected frame and the query were consumed exactly");
        const Reply rejected = reply_at(&fixture, offset);
        CHECK_U64(rejected.status, 1, cases[i].name);
        CHECK_U64(rejected.lba, cases[i].lba, "the rejected lba is echoed");
        CHECK_U64(rejected.count, 0, "a rejected request reports no blocks");
        CHECK_U64(reply_at(&fixture, offset + 16).op, 3, "the next frame is a query");
        CHECK_U64(reply_at(&fixture, offset + 16).status, 0, "the query is answered");
        offset += 16 + 24;
    }

    fixture_done(&fixture);
}

/* A count whose product with the block size does not fit 32 bits, or does not
 * fit the ring, must be refused promptly instead of being awaited forever. */
static void check_oversized_count(void)
{
    SECTION("counts that cannot be frames");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    struct {
        uint8_t op;
        uint32_t count;
        const char *name;
    } cases[] = {
        {1, UINT32_C(0x00100001), "read count * 4096 wraps 32 bits"},
        {1, UINT32_C(0x00100000), "read count * 4096 wraps to zero"},
        {1, UINT32_C(0xffffffff), "read count at the top of the range"},
        {1, 2, "two blocks do not fit the ring"},
        {2, UINT32_C(0x00100001), "write count * 4096 wraps 32 bits"},
        {2, UINT32_C(0x00200001), "write count * 4096 wraps round again"},
    };
    const uint32_t case_count = (uint32_t)(sizeof cases / sizeof cases[0]);

    uint32_t offset = 0;
    for (uint32_t i = 0; i < case_count; ++i) {
        /* Only the header is written: a count that cannot describe a frame has
         * to be refused from the header alone, because the payload it claims
         * could never fit the ring. */
        push_request(&fixture, cases[i].op, 0, (uint16_t)i, 0, cases[i].count, 0,
                     NULL);
        push_capacity_query(&fixture, (uint16_t)(0x80 + i));
        CHECK_U64(serve(&fixture), 2, cases[i].name);
        const Reply rejected = reply_at(&fixture, offset);
        CHECK_U64(rejected.status, 1, cases[i].name);
        CHECK_U64(rejected.count, 0, "no blocks are claimed for an impossible frame");
        CHECK_U64(reply_at(&fixture, offset + 16).op, 3, "the query still parses");
        CHECK_U64(reply_capacity(&fixture, offset + 16), FIXTURE_BLOCKS,
                  "the query is answered");
        offset += 16 + 24;
    }
    CHECK_U64(host_tail(&fixture), 16 * case_count + 16 * case_count,
              "only the headers were consumed");

    fixture_done(&fixture);
}

/* The failure path has to be reachable on demand, answered explicitly and must
 * not touch the store or invent data. A capacity query does not touch storage,
 * so it neither fails nor clears the flag. */
static void check_fault_injection(void)
{
    SECTION("an injected fault is explicit and all or nothing");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    fixture.block.fail_next = true;
    push_capacity_query(&fixture, 0x0701);
    CHECK_U64(serve(&fixture), 1, "the query is answered");
    CHECK_U64(reply_at(&fixture, 0).status, 0, "a query does not fail");
    CHECK_U64(reply_capacity(&fixture, 0), FIXTURE_BLOCKS, "the query is answered");
    CHECK(fixture.block.fail_next, "a query does not consume the fault flag");

    push_request(&fixture, 1, 0, 0x0702, 1, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 1, "the read is answered");
    const Reply failed = reply_at(&fixture, 24);
    CHECK_U64(failed.op, 1, "the failed read echoes its op");
    CHECK_U64(failed.status, 2, "the failed read reports a device fault");
    CHECK_U64(failed.count, 0, "the failed read claims no blocks");
    CHECK_U64(published(&fixture), 24 + 16, "the failure reply is a header only");
    CHECK(!fixture.block.fail_next, "the fault flag cleared");
    CHECK_U64(irq_status(&fixture), 1, "the failure also notifies");

    push_request(&fixture, 1, 0, 0x0703, 1, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 1, "the next read is answered");
    CHECK_U64(reply_at(&fixture, 40).status, 0, "the next read succeeds");
    CHECK_U64(published(&fixture), 40 + 16 + 4096, "the next read carries its block");

    /* A write that fails must not leave the store half written. */
    fixture_done(&fixture);
    fixture_init(&fixture, FIXTURE_BLOCKS);
    uint8_t payload[4096];
    memset(payload, 0x77, sizeof payload);
    fixture.block.fail_next = true;
    push_request(&fixture, 2, 0, 0x0801, 4, 1, 0, payload);
    push_capacity_query(&fixture, 0x0802);
    CHECK_U64(serve(&fixture), 2, "the failing write and the query are answered");
    CHECK_U64(reply_at(&fixture, 0).status, 2, "the write reports a device fault");
    CHECK_U64(host_tail(&fixture), 16 + 4096 + 16,
              "the failing write's payload is still consumed");
    int unchanged = 1;
    for (uint32_t at = 0; at < sizeof payload; ++at) {
        if (fixture.storage[4 * 4096 + at] != pattern(4, at)) {
            unchanged = 0;
            break;
        }
    }
    CHECK(unchanged, "a failed write leaves the block untouched");
    CHECK_U64(reply_at(&fixture, 16).op, 3, "the query after it still parses");
    fixture_done(&fixture);
}

/* The injected fault is spent by an *answer*, not by an attempt: a reply that
 * cannot be sent is not an answer. tools/host_block.h promises the fault to the
 * next request that touches storage, and a full response ring must not be able
 * to swallow it. Found by review after the first version cleared the flag
 * before the reply had any room. */
static void check_fault_waits_for_its_answer(void)
{
    SECTION("an injected fault survives a reply that cannot be sent");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    /* Leave seven bytes of room: less than the 16-byte header of any reply, so
     * nothing at all can be answered until the Guest drains. */
    const uint32_t occupied = (FIXTURE_RING - 1U) - 7U;
    CHECK(yan_transport_host_publish(&fixture.transport, occupied) == YAN_OK,
          "the response ring is filled to seven free bytes");
    CHECK_U64(yan_transport_host_writable(&fixture.transport), 7,
              "seven bytes are free");

    fixture.block.fail_next = true;
    push_request(&fixture, 1, 0, 0x0f01, 1, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 0, "no reply fits, so nothing is answered");
    CHECK_U64(host_tail(&fixture), 0, "and the request stays in the ring");
    CHECK(fixture.block.fail_next,
          "an answer that never went out does not spend the fault");

    /* The Guest drains the ring; now the request can be answered, and the
     * answer is the failure that was asked for. */
    guest_consume(&fixture, occupied);
    CHECK_U64(serve(&fixture), 1, "the request is answered once there is room");
    const Reply failed = reply_at(&fixture, 0);
    CHECK_U64(failed.status, 2, "the pending fault is the one that lands");
    CHECK_U64(failed.count, 0, "the failure claims no blocks");
    CHECK_U64(published(&fixture), 16, "the failure reply is a header only");
    CHECK(!fixture.block.fail_next, "and now the fault is spent");

    /* The channel is healthy afterwards: the fault was one-shot. */
    push_request(&fixture, 1, 0, 0x0f02, 1, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 1, "the next read is answered");
    CHECK_U64(reply_at(&fixture, 16).status, 0, "the next read succeeds");
    fixture_done(&fixture);
}

/* Back pressure: two read requests are ready but only one reply fits the
 * host-to-guest ring. The pump must answer one, publish nothing of the second
 * and leave the second request in place - a half reply with the line asserted
 * would be a frame the Guest can see but never complete. */
static void check_back_pressure(void)
{
    SECTION("a reply that does not fit is not half published");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    push_request(&fixture, 1, 0, 0x0a01, 0, 1, 0, NULL);
    push_request(&fixture, 1, 0, 0x0a02, 1, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 1, "only the first reply fits");
    CHECK_U64(host_tail(&fixture), 16, "the second request is not consumed");
    CHECK_U64(published(&fixture), 16 + 4096, "exactly one reply was published");
    uint8_t pending[16];
    published_bytes(&fixture, 16, pending, sizeof pending);
    CHECK_U64(serve(&fixture), 0, "a second pass has nothing more it can do");
    CHECK_U64(published(&fixture), 16 + 4096, "and publishes nothing more");
    uint8_t untouched[16];
    published_bytes(&fixture, 16, untouched, sizeof untouched);
    CHECK(memcmp(pending, untouched, sizeof pending) == 0,
          "the reply waiting in the ring is not written over");
    CHECK(!fixture.transport.overflow_detected, "and no overrun is latched");

    /* The Guest drains the first reply; now the second one fits, and its bytes
     * cross the wrap point of the ring. */
    guest_consume(&fixture, 16 + 4096);
    CHECK_U64(host_tail(&fixture), 16, "the second request is still pending");
    CHECK_U64(serve(&fixture), 1, "the second request is answered once there is room");
    CHECK_U64(host_tail(&fixture), 32, "the second request is consumed");
    CHECK_U64(published(&fixture), 16 + 4096, "one reply is pending again");
    const Reply second = reply_at(&fixture, 0);
    CHECK_U64(second.op, 1, "the wrapped reply echoes its op");
    CHECK_U64(second.tag, 0x0a02, "the wrapped reply echoes its tag");
    CHECK_U64(second.lba, 1, "the wrapped reply echoes its lba");
    uint8_t block[4096];
    published_bytes(&fixture, 16, block, sizeof block);
    int matches = 1;
    for (uint32_t at = 0; at < sizeof block; ++at) {
        if (block[at] != pattern(1, at)) {
            matches = 0;
            break;
        }
    }
    CHECK(matches, "the wrapped payload is block 1's content");

    fixture_done(&fixture);
}

/* The payload of a write may arrive after its header; nothing is consumed until
 * the frame is complete. */
static void check_partial_payload(void)
{
    SECTION("a payload that arrives late");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    uint8_t payload[4096];
    for (uint32_t at = 0; at < sizeof payload; ++at) {
        payload[at] = (uint8_t)(at ^ 0x3cU);
    }
    uint8_t header[16];
    build_header(header, 2, 0, 0x0b01, 5, 1, 0);
    guest_push(&fixture, header, 16);
    guest_push(&fixture, payload, 100);
    CHECK_U64(serve(&fixture), 0, "an incomplete frame answers nothing");
    CHECK_U64(host_tail(&fixture), 0, "an incomplete frame consumes nothing");
    CHECK_U64(published(&fixture), 0, "an incomplete frame publishes nothing");
    guest_push(&fixture, payload + 100, sizeof payload - 100);
    CHECK_U64(serve(&fixture), 1, "the completed frame is answered");
    CHECK_U64(host_tail(&fixture), 16 + 4096, "the completed frame is consumed");
    CHECK_U64(reply_at(&fixture, 0).status, 0, "the write is accepted");
    int stored = 1;
    for (uint32_t at = 0; at < sizeof payload; ++at) {
        if (fixture.storage[5 * 4096 + at] != payload[at]) {
            stored = 0;
            break;
        }
    }
    CHECK(stored, "the late payload reached the backing store");

    fixture_done(&fixture);
}

/* A capacity of zero is a valid configuration and answers reads and writes as
 * out of range; the query itself reports the truth. */
static void check_zero_capacity(void)
{
    SECTION("a device of zero blocks");
    Fixture fixture;
    fixture_init(&fixture, 0);

    push_request(&fixture, 1, 0, 0x0c01, 0, 1, 0, NULL);
    push_capacity_query(&fixture, 0x0c02);
    CHECK_U64(serve(&fixture), 2, "both frames are answered");
    CHECK_U64(reply_at(&fixture, 0).status, 1, "a read on a zero-block device is refused");
    CHECK_U64(reply_at(&fixture, 16).status, 0, "the query is answered");
    CHECK_U64(reply_capacity(&fixture, 16), 0, "the capacity is zero");

    fixture_done(&fixture);
}

/* The init contract, including the argument the device is allowed to have:
 * no store at all with a capacity of zero. */
static void check_init_contract(void)
{
    SECTION("the init contract");
    uint8_t byte = 0;
    CHECK_U64(yan_host_block_init(NULL, &byte, 1), YAN_INVALID_ARGUMENT,
              "a NULL block is rejected");
    YanHostBlock block = {0};
    CHECK_U64(yan_host_block_init(&block, NULL, 1), YAN_INVALID_ARGUMENT,
              "storage is required for a non-empty device");
    CHECK_U64(yan_host_block_init(&block, NULL, 0), YAN_OK,
              "a zero-block device needs no storage");
    CHECK_U64(block.served, 0, "the served counter starts at zero");
    CHECK(!block.fail_next, "fault injection starts off");
}

/* A ring region that does not lie inside the RAM the caller handed over is not
 * dereferenced: the pump reports that it has nothing to do. */
static void check_ram_window_guard(void)
{
    SECTION("a window that does not contain the rings");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);
    push_capacity_query(&fixture, 0x0d01);
    CHECK_U64(yan_host_block_service(&fixture.block, &fixture.transport,
                                     &fixture.ram,
                                     FIXTURE_BASE + FIXTURE_RAM - 4096), 0,
              "a window below the rings answers nothing");
    CHECK_U64(host_tail(&fixture), 0, "and consumes nothing");
    push_capacity_query(&fixture, 0x0d02);
    /* Both requests are still in the ring: the guard consumed none of them. */
    CHECK_U64(serve(&fixture), 2, "the requests work with the right window");
    fixture_done(&fixture);
}

/* The boundary of the capacity check: the last block is readable and the one
 * after it is not. An off-by-one here is invisible until a caller asks for the
 * last block of a real device. */
static void check_last_block(void)
{
    SECTION("the last block is inside the device");
    Fixture fixture;
    fixture_init(&fixture, FIXTURE_BLOCKS);

    push_request(&fixture, 1, 0, 0x0e01, (uint32_t)FIXTURE_BLOCKS - 1, 1, 0, NULL);
    push_capacity_query(&fixture, 0x0e02);
    CHECK_U64(serve(&fixture), 2, "the last block is readable");
    const Reply last = reply_at(&fixture, 0);
    CHECK_U64(last.status, 0, "the last block is served");
    CHECK_U64(last.lba, (uint32_t)FIXTURE_BLOCKS - 1, "the last lba is echoed");
    uint8_t block[4096];
    published_bytes(&fixture, 16, block, sizeof block);
    int matches = 1;
    for (uint32_t at = 0; at < sizeof block; ++at) {
        if (block[at] != pattern((uint32_t)FIXTURE_BLOCKS - 1, at)) {
            matches = 0;
            break;
        }
    }
    CHECK(matches, "the last block carries its own content");
    CHECK_U64(reply_at(&fixture, 16).op, 3, "the query after it is answered");

    push_request(&fixture, 1, 0, 0x0e03, (uint32_t)FIXTURE_BLOCKS, 1, 0, NULL);
    CHECK_U64(serve(&fixture), 1, "one block past the end is answered");
    /* After the read's header and payload and the query's header and capacity. */
    CHECK_U64(reply_at(&fixture, 16 + 4096 + 24).status, 1, "and refused");
    fixture_done(&fixture);
}

int main(void)
{
    check_partial_header();
    check_header_split();
    check_read_whole_frame();
    check_write_then_query();
    check_invalid_frames_consumed();
    check_parameter_errors();
    check_oversized_count();
    check_fault_injection();
    check_fault_waits_for_its_answer();
    check_back_pressure();
    check_partial_payload();
    check_zero_capacity();
    check_init_contract();
    check_ram_window_guard();
    check_last_block();

    printf("%u check(s), %u failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
