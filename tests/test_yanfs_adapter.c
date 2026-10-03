/* Host Unity test for the Guest YanFS block adapter (os/yanfs_block.c).
 *
 * The test is Linux only on purpose: the mock transport keeps the response ring
 * at a fixed low address so the 32-bit ring base the adapter reads is a real
 * host address. The adapter source is compiled into this translation unit after
 * the platform header is suppressed, so the adapter is driven for real while
 * the platform, the block framing callbacks and the cooperative wait are the
 * test's own. block.h and task.h keep their real declarations; no production
 * file is changed and there is no production test switch.
 *
 * What this suite proves: the adapter's channel preconditions, request pairing,
 * the read-only readiness predicate, the status/shape mapping and the
 * fail-closed latches. It does not prove that the runtime's task_wait works:
 * the wait here is a mock that supplies bytes in a controlled order. That part
 * is the job of the Guest runtime tests.
 */
#define _GNU_SOURCE 1

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "unity.h"
#include "../os/block.h"
#include "../os/task.h"
#include "../os/yanfs.h"
#include "../os/yanfs_block.h"

/* ------------------------------------------------------- mock platform view */

/* Suppress os/platform.h inside the included adapter source. */
#define YAN_OS_PLATFORM_H 1

#define YAN_OS_TRANSPORT_STATUS_HOST_READY UINT32_C(0x00000001)
#define YAN_OS_TRANSPORT_STATUS_OVERFLOW_DETECTED UINT32_C(0x00000008)

#define MOCK_RING_ADDRESS UINT32_C(0x30000000)
#define MOCK_RING_SIZE UINT32_C(8192)
#define MOCK_MAX_RING_SIZE UINT32_C(16384)
#define MOCK_MAP_BYTES (2u * MOCK_MAX_RING_SIZE)

static uint8_t *mock_map;
static uint32_t mock_status;
static uint32_t mock_ring_base;
static uint32_t mock_ring_size;
static uint32_t mock_g2h_head;
static uint32_t mock_g2h_tail;
static uint32_t mock_h2g_head;
static uint32_t mock_h2g_tail;

static int yan_os_transport_host_ready(void)
{
    return (mock_status & YAN_OS_TRANSPORT_STATUS_HOST_READY) != 0u;
}

static uint32_t yan_os_transport_status(void)
{
    return mock_status;
}

static uint32_t yan_os_transport_ring_base(void)
{
    return mock_ring_base;
}

static uint32_t yan_os_transport_ring_size(void)
{
    return mock_ring_size;
}

static uint32_t yan_os_transport_g2h_head(void)
{
    return mock_g2h_head;
}

static uint32_t yan_os_transport_g2h_tail(void)
{
    return mock_g2h_tail;
}

static uint32_t yan_os_transport_h2g_tail(void)
{
    return mock_h2g_tail;
}

static uint32_t yan_os_transport_h2g_available(void)
{
    if (mock_ring_size == 0u) {
        return 0u;
    }
    return (mock_h2g_head - mock_h2g_tail) & (mock_ring_size - 1u);
}

static uint32_t yan_os_ring_index(uint32_t base, uint32_t size, uint32_t i)
{
    return (base + i) & (size - 1u);
}

static uint8_t *mock_ring_bytes(void)
{
    return (uint8_t *)(uintptr_t)(mock_ring_base + mock_ring_size);
}

static uint8_t mock_ring_byte(uint32_t index)
{
    return mock_ring_bytes()[yan_os_ring_index(mock_h2g_tail, mock_ring_size, index)];
}

static void mock_peek_copy(uint8_t *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = mock_ring_byte(i);
    }
}

static void mock_consume(uint8_t *out, uint32_t count)
{
    if (out != NULL) {
        mock_peek_copy(out, count);
    }
    mock_h2g_tail = (mock_h2g_tail + count) & (mock_ring_size - 1u);
}

static void mock_stage(const uint8_t *bytes, uint32_t count)
{
    uint8_t *ring = mock_ring_bytes();
    for (uint32_t i = 0; i < count; ++i) {
        ring[yan_os_ring_index(mock_h2g_head, mock_ring_size, i)] = bytes[i];
    }
    mock_h2g_head = (mock_h2g_head + count) & (mock_ring_size - 1u);
}

/* ------------------------------------------------------- mock framing layer */

static uint32_t mock_max_count;
static int mock_submit_result;
static YanOsBlockRequest mock_request;
static const uint8_t *mock_request_data;
static uint32_t mock_submit_calls;
static uint32_t mock_take_calls;

static uint16_t mock_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t mock_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

uint32_t yan_os_block_max_count(void)
{
    return mock_max_count;
}

int yan_os_block_submit(const YanOsBlockRequest *request, const uint8_t *data)
{
    ++mock_submit_calls;
    if (request != NULL) {
        mock_request = *request;
    }
    mock_request_data = data;
    return mock_submit_result;
}

int yan_os_block_take(YanOsBlockResponse *response, uint8_t *data,
                      uint32_t capacity, uint32_t *length)
{
    ++mock_take_calls;
    if (length != NULL) {
        *length = 0u;
    }
    if (!yan_os_transport_host_ready() || mock_ring_size == 0u) {
        return YAN_OS_BLOCK_INVALID;
    }
    const uint32_t available = yan_os_transport_h2g_available();
    if (available < YAN_OS_BLOCK_HEADER_SIZE) {
        return YAN_OS_BLOCK_AGAIN;
    }
    uint8_t header[YAN_OS_BLOCK_HEADER_SIZE];
    mock_peek_copy(header, sizeof header);
    const uint8_t op = header[0];
    const uint8_t status = header[1];
    uint64_t payload = 0u;
    int malformed = 0;
    if (op == YAN_OS_BLOCK_OP_READ) {
        payload = status == YAN_OS_BLOCK_STATUS_OK
                      ? (uint64_t)mock_le32(header + 8) * YAN_OS_BLOCK_BLOCK_SIZE
                      : 0u;
    } else if (op == YAN_OS_BLOCK_OP_WRITE) {
        payload = 0u;
    } else if (op == YAN_OS_BLOCK_OP_CAPACITY) {
        payload = status == YAN_OS_BLOCK_STATUS_OK ? 8u : 0u;
    } else {
        malformed = 1;
    }
    if (mock_le32(header + 12) != 0u) {
        malformed = 1;
    }
    const uint64_t total = (uint64_t)YAN_OS_BLOCK_HEADER_SIZE + payload;
    if (total > (uint64_t)(mock_ring_size - 1u)) {
        mock_consume(NULL, YAN_OS_BLOCK_HEADER_SIZE);
        return YAN_OS_BLOCK_MALFORMED;
    }
    if ((uint64_t)available < total) {
        return YAN_OS_BLOCK_AGAIN;
    }
    if (payload > (uint64_t)capacity || (payload != 0u && data == NULL)) {
        return YAN_OS_BLOCK_INVALID;
    }
    if (malformed != 0) {
        mock_consume(NULL, (uint32_t)total);
        return YAN_OS_BLOCK_MALFORMED;
    }
    uint8_t taken[YAN_OS_BLOCK_HEADER_SIZE];
    mock_consume(taken, sizeof taken);
    if (payload != 0u) {
        mock_consume(data, (uint32_t)payload);
    }
    if (response != NULL) {
        response->op = taken[0];
        response->status = taken[1];
        response->tag = mock_le16(taken + 2);
        response->lba = mock_le32(taken + 4);
        response->count = mock_le32(taken + 8);
    }
    if (length != NULL) {
        *length = (uint32_t)payload;
    }
    return 0;
}

/* ---------------------------------------------------------- mock task_wait */

static uint32_t mock_wait_calls;
static bool mock_wait_called_predicate;
static int mock_wait_predicate_result;
static bool mock_wait_predicate_pure;
static bool mock_wait_saw_ready;
static void (*mock_wait_step)(uint32_t call);

static uint32_t snap_status;
static uint32_t snap_g2h_head;
static uint32_t snap_g2h_tail;
static uint32_t snap_h2g_head;
static uint32_t snap_h2g_tail;
static uint8_t snap_ring[MOCK_MAX_RING_SIZE];

static void mock_snapshot(void)
{
    snap_status = mock_status;
    snap_g2h_head = mock_g2h_head;
    snap_g2h_tail = mock_g2h_tail;
    snap_h2g_head = mock_h2g_head;
    snap_h2g_tail = mock_h2g_tail;
    memcpy(snap_ring, mock_ring_bytes(), mock_ring_size);
}

static bool mock_state_unchanged(void)
{
    return snap_status == mock_status &&
           snap_g2h_head == mock_g2h_head && snap_g2h_tail == mock_g2h_tail &&
           snap_h2g_head == mock_h2g_head && snap_h2g_tail == mock_h2g_tail &&
           memcmp(snap_ring, mock_ring_bytes(), mock_ring_size) == 0;
}

/* True when the frame at the consumer position is a complete 16-byte error
 * response. 0021 says a non-zero status carries the header and nothing else, so
 * the adapter must never wait on one. This guard turns a predicate mutation
 * that waits for a payload that cannot come into an assertion failure instead of
 * a hung test; it never fires for the correct adapter. */
static bool mock_complete_error_frame(void)
{
    if (yan_os_transport_h2g_available() < YAN_OS_BLOCK_HEADER_SIZE) {
        return false;
    }
    uint8_t header[YAN_OS_BLOCK_HEADER_SIZE];
    mock_peek_copy(header, sizeof header);
    if (mock_le32(header + 12) != 0u) {
        return false;
    }
    const uint8_t op = header[0];
    const uint8_t status = header[1];
    const uint32_t lba = mock_le32(header + 4);
    const uint32_t count = mock_le32(header + 8);
    if (status == YAN_OS_BLOCK_STATUS_OK || count != 0u) {
        return false;
    }
    if (op == YAN_OS_BLOCK_OP_CAPACITY && lba != 0u) {
        return false;
    }
    return op == YAN_OS_BLOCK_OP_READ || op == YAN_OS_BLOCK_OP_WRITE ||
           op == YAN_OS_BLOCK_OP_CAPACITY;
}

void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)
{
    (void)event;
    ++mock_wait_calls;
    mock_snapshot();
    const int ready = predicate(context);
    mock_wait_called_predicate = true;
    mock_wait_predicate_result = ready;
    mock_wait_predicate_pure = mock_state_unchanged();
    if (ready != 0) {
        mock_wait_saw_ready = true;
        return;
    }
    if (mock_complete_error_frame()) {
        TEST_FAIL_MESSAGE(
            "the adapter must not wait on a complete 16-byte error frame");
    }
    if (mock_wait_step != NULL) {
        mock_wait_step(mock_wait_calls);
    }
}

/* ------------------------------------------------------------- adapter source */

#include "../os/yanfs_block.c"

/* ------------------------------------------------------------- test helpers */

static YanFsBlockAdapter adapter;
static uint8_t test_data[YAN_OS_BLOCK_BLOCK_SIZE];
static uint8_t frame_header[YAN_OS_BLOCK_HEADER_SIZE];
static const uint8_t *frame_payload;
static uint32_t frame_payload_length;

static void build_header(uint8_t out[16], uint8_t op, uint8_t status, uint16_t tag,
                         uint32_t lba, uint32_t count)
{
    memset(out, 0, 16u);
    out[0] = op;
    out[1] = status;
    out[2] = (uint8_t)tag;
    out[3] = (uint8_t)(tag >> 8);
    out[4] = (uint8_t)lba;
    out[5] = (uint8_t)(lba >> 8);
    out[6] = (uint8_t)(lba >> 16);
    out[7] = (uint8_t)(lba >> 24);
    out[8] = (uint8_t)count;
    out[9] = (uint8_t)(count >> 8);
    out[10] = (uint8_t)(count >> 16);
    out[11] = (uint8_t)(count >> 24);
}

static void stage_header(uint8_t op, uint8_t status, uint16_t tag, uint32_t lba,
                         uint32_t count)
{
    uint8_t header[16];
    build_header(header, op, status, tag, lba, count);
    mock_stage(header, sizeof header);
}

static void reset_ring(void)
{
    mock_g2h_head = 0u;
    mock_g2h_tail = 0u;
    mock_h2g_head = 0u;
    mock_h2g_tail = 0u;
    memset(mock_map, 0, MOCK_MAP_BYTES);
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_block_init(&adapter));
}

static void reinit_ok(void)
{
    memset(&adapter, 0, sizeof adapter);
    init_ok();
}

static YanFsBlockIo backend(void)
{
    return yan_fs_block_backend(&adapter);
}

static void stage_rest_after_wait(uint32_t call)
{
    (void)call;
    mock_stage(frame_header + 8u, 8u);
    mock_stage(frame_payload, frame_payload_length);
}

static void stage_split_frame(uint32_t call)
{
    if (call == 1u) {
        mock_stage(frame_header + 1u, 15u);
    } else if (call == 2u) {
        mock_stage(frame_payload, frame_payload_length);
    }
}

void setUp(void)
{
    if (mock_map == NULL) {
        void *mapped = mmap((void *)(uintptr_t)MOCK_RING_ADDRESS, MOCK_MAP_BYTES,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mapped == MAP_FAILED) {
            fputs("HARNESS-ERROR: cannot map the mock transport ring\n", stderr);
            exit(2);
        }
        mock_map = (uint8_t *)mapped;
    }
    mock_status = YAN_OS_TRANSPORT_STATUS_HOST_READY;
    mock_ring_base = MOCK_RING_ADDRESS;
    mock_ring_size = MOCK_RING_SIZE;
    reset_ring();
    mock_max_count = 1u;
    mock_submit_result = 0;
    mock_request = (YanOsBlockRequest){0};
    mock_request_data = NULL;
    mock_submit_calls = 0u;
    mock_take_calls = 0u;
    mock_wait_calls = 0u;
    mock_wait_called_predicate = false;
    mock_wait_predicate_result = 0;
    mock_wait_predicate_pure = false;
    mock_wait_saw_ready = false;
    mock_wait_step = NULL;
    memset(&adapter, 0, sizeof adapter);
    memset(test_data, 0, sizeof test_data);
    memset(frame_header, 0, sizeof frame_header);
    frame_payload = NULL;
    frame_payload_length = 0u;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------- tests */

static void init_rejects_null_and_channel_preconditions(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_block_init(NULL));

    mock_status = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_block_init(&adapter));

    mock_status = YAN_OS_TRANSPORT_STATUS_HOST_READY;
    mock_max_count = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_block_init(&adapter));

    mock_max_count = 1u;
    mock_status |= YAN_OS_TRANSPORT_STATUS_OVERFLOW_DETECTED;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_block_init(&adapter));

    mock_status = YAN_OS_TRANSPORT_STATUS_HOST_READY;
    mock_g2h_head = 1u;
    mock_g2h_tail = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_block_init(&adapter));

    mock_g2h_head = mock_g2h_tail = 0u;
    mock_stage((const uint8_t *)"x", 1u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_block_init(&adapter));

    reset_ring();
    const uint32_t g2h = mock_g2h_head;
    const uint32_t h2g = mock_h2g_tail;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_block_init(&adapter));
    TEST_ASSERT_TRUE(adapter.initialized);
    TEST_ASSERT_FALSE(adapter.busy);
    TEST_ASSERT_FALSE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT16(0u, adapter.next_tag);
    TEST_ASSERT_EQUAL_UINT8(0u, adapter.pending_op);
    TEST_ASSERT_EQUAL_UINT32(0u, adapter.pending_tag);
    TEST_ASSERT_EQUAL_UINT32(0u, adapter.pending_lba);
    TEST_ASSERT_EQUAL_UINT32(g2h, mock_g2h_head);
    TEST_ASSERT_EQUAL_UINT32(h2g, mock_h2g_tail);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_submit_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_wait_calls);
}

static void init_rejects_busy_and_failed_reinit(void)
{
    init_ok();
    adapter.busy = true;
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, yan_fs_block_init(&adapter));

    adapter.busy = false;
    adapter.failed = true;
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_block_init(&adapter));

    adapter.failed = false;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_block_init(&adapter));
}

static void backend_callbacks_reject_uninitialized_and_failed(void)
{
    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    uint8_t buffer[YAN_OS_BLOCK_BLOCK_SIZE];

    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, buffer));
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.write_block(io.context, 0u, buffer));

    init_ok();
    adapter.failed = true;
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
}

static void capacity_success_decodes_payload(void)
{
    init_ok();
    const uint8_t payload[8] = {0x11u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    mock_stage(payload, sizeof payload);

    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.capacity(io.context, &blocks));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(17), blocks);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_submit_calls);
    TEST_ASSERT_EQUAL_UINT8(YAN_OS_BLOCK_OP_CAPACITY, mock_request.op);
    TEST_ASSERT_EQUAL_UINT8(0u, mock_request.flags);
    TEST_ASSERT_EQUAL_UINT16(0u, mock_request.tag);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_request.lba);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_request.count);
    TEST_ASSERT_EQUAL_UINT16(1u, adapter.next_tag);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_take_calls);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, mock_wait_calls,
                                     "a fast-ready frame must not wait");
    TEST_ASSERT_FALSE(adapter.failed);
    TEST_ASSERT_FALSE(adapter.busy);
}

static void read_success_copies_block(void)
{
    init_ok();
    for (uint32_t i = 0; i < sizeof test_data; ++i) {
        test_data[i] = (uint8_t)(i * 3u + 1u);
    }
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 2u, 1u);
    mock_stage(test_data, sizeof test_data);

    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.read_block(io.context, 2u, out));
    TEST_ASSERT_EQUAL_MEMORY(test_data, out, sizeof test_data);
    TEST_ASSERT_EQUAL_UINT8(YAN_OS_BLOCK_OP_READ, mock_request.op);
    TEST_ASSERT_EQUAL_UINT32(2u, mock_request.lba);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_request.count);
    TEST_ASSERT_EQUAL_UINT16(1u, adapter.next_tag);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_wait_calls);
}

static void write_success_records_request(void)
{
    init_ok();
    memset(test_data, 0x5a, sizeof test_data);
    stage_header(YAN_OS_BLOCK_OP_WRITE, YAN_OS_BLOCK_STATUS_OK, 0u, 3u, 1u);

    YanFsBlockIo io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.write_block(io.context, 3u, test_data));
    TEST_ASSERT_EQUAL_UINT8(YAN_OS_BLOCK_OP_WRITE, mock_request.op);
    TEST_ASSERT_EQUAL_UINT32(3u, mock_request.lba);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_request.count);
    TEST_ASSERT_EQUAL_PTR(test_data, mock_request_data);
    TEST_ASSERT_EQUAL_UINT16(1u, adapter.next_tag);
}

static void tag_wraps_after_submit(void)
{
    init_ok();
    adapter.next_tag = UINT16_C(0xffff);
    const uint8_t payload[8] = {1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, UINT16_C(0xffff),
                 0u, 0u);
    mock_stage(payload, sizeof payload);

    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.capacity(io.context, &blocks));
    TEST_ASSERT_EQUAL_UINT16(UINT16_C(0xffff), mock_request.tag);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, adapter.next_tag, "u16 tag wraps");
}

static void partial_frame_waits_once_and_predicate_is_pure(void)
{
    init_ok();
    for (uint32_t i = 0; i < sizeof test_data; ++i) {
        test_data[i] = (uint8_t)(0x20u + i);
    }
    frame_payload = test_data;
    frame_payload_length = sizeof test_data;
    build_header(frame_header, YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u,
                 1u);
    mock_stage(frame_header, 8u);
    mock_wait_step = stage_rest_after_wait;

    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    memset(out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.read_block(io.context, 0u, out));
    TEST_ASSERT_EQUAL_MEMORY(test_data, out, sizeof test_data);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_wait_calls);
    TEST_ASSERT_TRUE(mock_wait_called_predicate);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, mock_wait_predicate_result,
        "the predicate must say not-ready before the frame is completed");
    TEST_ASSERT_TRUE_MESSAGE(mock_wait_predicate_pure,
                             "the predicate must not change any channel state");
    TEST_ASSERT_FALSE(mock_wait_saw_ready);
}

static void split_header_then_payload_waits_twice(void)
{
    init_ok();
    for (uint32_t i = 0; i < sizeof test_data; ++i) {
        test_data[i] = (uint8_t)(0x40u + i);
    }
    frame_payload = test_data;
    frame_payload_length = sizeof test_data;
    build_header(frame_header, YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u,
                 1u);
    mock_stage(frame_header, 1u);
    mock_wait_step = stage_split_frame;

    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    memset(out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.read_block(io.context, 0u, out));
    TEST_ASSERT_EQUAL_MEMORY(test_data, out, sizeof test_data);
    TEST_ASSERT_EQUAL_UINT32(2u, mock_wait_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_wait_predicate_result);
    TEST_ASSERT_TRUE(mock_wait_predicate_pure);
    TEST_ASSERT_FALSE(mock_wait_saw_ready);
}

static void ring_wrap_header_is_read_correctly(void)
{
    init_ok();
    mock_h2g_head = MOCK_RING_SIZE - 8u;
    mock_h2g_tail = MOCK_RING_SIZE - 8u;
    const uint8_t payload[8] = {0x22u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    mock_stage(payload, sizeof payload);

    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.capacity(io.context, &blocks));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x22), blocks);
}

static void response_ready_classifies_headers(void)
{
    reset_ring();
    TEST_ASSERT_EQUAL_INT(0, response_ready(NULL));

    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_DEVICE, 0u, 0u, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, response_ready(NULL),
        "a 16-byte device-error read frame is complete without a payload");
    reset_ring();

    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, response_ready(NULL),
        "a success read with only the header staged is not ready");
    reset_ring();

    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 2u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, response_ready(NULL),
        "a count no frame in this ring can hold is reported ready, not waited");
    reset_ring();

    stage_header((uint8_t)9u, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, response_ready(NULL),
                                  "an unknown op is ready so the flow faults");
    reset_ring();

    uint8_t header[16];
    build_header(header, YAN_OS_BLOCK_OP_WRITE, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    header[12] = 1u; /* reserved must be zero */
    mock_stage(header, sizeof header);
    TEST_ASSERT_EQUAL_INT(1, response_ready(NULL));
    reset_ring();

    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(0, response_ready(NULL));
    const uint8_t payload[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    mock_stage(payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(1, response_ready(NULL));
    reset_ring();

    /* Shape violations are ready immediately, never a reason to sleep. */
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, response_ready(NULL),
        "a capacity success claiming count 1 must not wait for 8 bytes");
    reset_ring();
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 1u, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, response_ready(NULL),
        "a capacity success with lba 1 must not wait for 8 bytes");
    reset_ring();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 2u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, response_ready(NULL),
        "a read success claiming two blocks must not wait for 8192 bytes");
    reset_ring();
    stage_header(YAN_OS_BLOCK_OP_WRITE, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 2u);
    TEST_ASSERT_EQUAL_INT(1, response_ready(NULL));
    reset_ring();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_DEVICE, 0u, 0u, 1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, response_ready(NULL),
        "an error frame claiming a payload must not wait for it");
    reset_ring();
}

static void capacity_bad_shape_header_faults_without_waiting(void)
{
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, mock_wait_calls,
        "a capacity header claiming 8 payload bytes must not sleep for them");

    /* The rejected bad-shape header is deliberately left in the ring: take
     * returned AGAIN because it claimed 8 payload bytes that are not there. A
     * second independent fixture therefore starts from an empty ring, and the
     * wait counter is reset so the "must not sleep" claim is about this
     * request. */
    reset_ring();
    reinit_ok();
    mock_wait_calls = 0u;
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 1u, 0u);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_wait_calls);
}

static void read_success_count_two_on_large_ring_is_rejected_without_waiting(void)
{
    init_ok();
    /* A ring this large could hold an 8192-byte payload; the point is that the
     * shape violation is rejected before deriving any length, so the adapter
     * never sleeps for a two-block read it did not ask for. */
    mock_ring_size = MOCK_MAX_RING_SIZE;
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 2u);

    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, mock_wait_calls,
        "a two-block success header must be rejected, not waited out");
}

static void non_read_request_consumes_complete_read_frame_then_protocol(void)
{
    for (uint32_t i = 0; i < sizeof test_data; ++i) {
        test_data[i] = (uint8_t)(0x70u + i);
    }

    /* A complete, legitimate READ response answering a CAPACITY request: it must
     * be taken whole (0021) and only then rejected on the op. */
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    mock_stage(test_data, sizeof test_data);
    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_take_calls);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, yan_os_transport_h2g_available(),
        "the complete READ frame must be consumed before the rejection");

    /* Same for a WRITE request: its take buffer is 0 bytes, so the discard path
     * is what lets the 4112-byte frame leave the ring. */
    reinit_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    mock_stage(test_data, sizeof test_data);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL,
                          io.write_block(io.context, 0u, test_data));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_os_transport_h2g_available());
}

static void pending_write_consumes_complete_capacity_frame_then_protocol(void)
{
    uint8_t count_payload[8];
    for (uint32_t i = 0; i < sizeof count_payload; ++i) {
        count_payload[i] = (uint8_t)(0x11u + i);
    }
    for (uint32_t i = 0; i < sizeof test_data; ++i) {
        test_data[i] = (uint8_t)(0x30u + i);
    }

    /* A WRITE request's take buffer is 0 bytes, but the 24-byte CAPACITY frame
     * answering someone else is still a complete legitimate frame: 0021 takes
     * it whole and only then rejects the op. */
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    mock_stage(count_payload, sizeof count_payload);
    YanFsBlockIo io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL,
                          io.write_block(io.context, 0u, test_data));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, mock_wait_calls,
        "a complete 24-byte CAPACITY response must not be waited on");
    TEST_ASSERT_EQUAL_UINT32(1u, mock_take_calls);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, yan_os_transport_h2g_available(),
        "the complete CAPACITY frame and its 8-byte payload must be consumed");

    /* The adapter's own capacity query keeps the payload: the same frame shape
     * must still be decoded into the block count, not redirected wholesale. */
    reinit_ok();
    const uint8_t capacity_bytes[8] = {3u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    mock_stage(capacity_bytes, sizeof capacity_bytes);
    io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_OK, io.capacity(io.context, &blocks));
    TEST_ASSERT_EQUAL_UINT64(3u, blocks);
}

static void read_device_error_is_io_without_waiting(void)
{
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_DEVICE, 0u, 1u, 0u);

    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_ERROR, io.read_block(io.context, 1u, out));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, mock_wait_calls,
        "a 16-byte read error is complete; waiting would deadlock");
    TEST_ASSERT_EQUAL_UINT32(1u, mock_take_calls);
    TEST_ASSERT_FALSE(adapter.failed);
    TEST_ASSERT_FALSE(adapter.busy);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, out[0]);
}

static void status_one_and_three_fault(void)
{
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_INVALID, 0u, 0u, 0u);
    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);

    reinit_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_UNSUPPORTED, 0u, 0u, 0u);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);
}

static void wrong_tag_full_frame_is_consumed_then_rejected(void)
{
    init_ok();
    for (uint32_t i = 0; i < sizeof test_data; ++i) {
        test_data[i] = (uint8_t)(0x60u + i);
    }
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, UINT16_C(0x1234), 0u,
                 1u);
    mock_stage(test_data, sizeof test_data);

    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(1u, mock_take_calls);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, yan_os_transport_h2g_available(),
        "the whole mismatched frame must be consumed before it is rejected");

    /* The same rule for a legitimate small payload: an 8-byte CAPACITY frame
     * with a wrong tag is consumed whole (header and payload) before the tag is
     * rejected. */
    reinit_ok();
    uint8_t count_payload[8] = {0u};
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK,
                 UINT16_C(0x4321), 0u, 0u);
    mock_stage(count_payload, sizeof count_payload);
    io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, yan_os_transport_h2g_available(),
        "the small mismatched frame must be consumed before it is rejected");
}

static void wrong_lba_count_and_op_are_rejected(void)
{
    YanFsBlockIo io;
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];

    init_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 5u, 1u);
    mock_stage(test_data, sizeof test_data);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));

    reinit_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));

    reinit_ok();
    stage_header(YAN_OS_BLOCK_OP_WRITE, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));

    /* A legitimate wrong-op frame with a small payload is consumed whole too. */
    reinit_ok();
    uint8_t count_payload[8] = {0u};
    stage_header(YAN_OS_BLOCK_OP_CAPACITY, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    mock_stage(count_payload, sizeof count_payload);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, yan_os_transport_h2g_available(),
        "a wrong-op CAPACITY frame is consumed whole, payload included");
}

static void malformed_headers_fault(void)
{
    YanFsBlockIo io;
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];

    init_ok();
    stage_header((uint8_t)9u, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 0u);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);

    reinit_ok();
    uint8_t header[16];
    build_header(header, YAN_OS_BLOCK_OP_WRITE, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 1u);
    header[12] = 1u;
    mock_stage(header, sizeof header);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);

    reinit_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_OK, 0u, 0u, 2u);
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);
}

static void device_error_with_count_is_protocol(void)
{
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_DEVICE, 0u, 0u, 1u);
    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_TRUE(adapter.failed);
}

static void submit_again_or_invalid_faults_without_waiting(void)
{
    init_ok();
    mock_submit_result = YAN_OS_BLOCK_AGAIN;
    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, mock_wait_calls,
        "an AGAIN submit must fail closed, not wait for G2H space");
    TEST_ASSERT_EQUAL_UINT32(0u, mock_take_calls);

    reinit_ok();
    mock_submit_result = YAN_OS_BLOCK_INVALID;
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_wait_calls);
}

static void protocol_failure_stops_further_io(void)
{
    init_ok();
    stage_header(YAN_OS_BLOCK_OP_READ, YAN_OS_BLOCK_STATUS_INVALID, 0u, 0u, 0u);
    YanFsBlockIo io = backend();
    uint8_t out[YAN_OS_BLOCK_BLOCK_SIZE];
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    const uint32_t submits = mock_submit_calls;
    const uint32_t takes = mock_take_calls;

    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.read_block(io.context, 0u, out));
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_EQUAL_UINT32(submits, mock_submit_calls);
    TEST_ASSERT_EQUAL_UINT32(takes, mock_take_calls);
}

static void busy_reentry_is_protocol(void)
{
    init_ok();
    adapter.busy = true;
    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE_MESSAGE(adapter.failed,
                             "a reentrant request must disable the adapter");
}

static void per_request_configuration_change_faults(void)
{
    init_ok();
    mock_status = 0u;
    YanFsBlockIo io = backend();
    uint64_t blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_submit_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_wait_calls);

    /* Reset the channel before re-initialising: init itself reads the standing
     * state, so a still-cleared mock_status would fail the init, not the
     * request under test. */
    mock_status = YAN_OS_TRANSPORT_STATUS_HOST_READY;
    reinit_ok();
    mock_status = YAN_OS_TRANSPORT_STATUS_HOST_READY |
                  YAN_OS_TRANSPORT_STATUS_OVERFLOW_DETECTED;
    io = backend();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO_PROTOCOL, io.capacity(io.context, &blocks));
    TEST_ASSERT_TRUE(adapter.failed);
    TEST_ASSERT_EQUAL_UINT32(0u, mock_submit_calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(init_rejects_null_and_channel_preconditions);
    RUN_TEST(init_rejects_busy_and_failed_reinit);
    RUN_TEST(backend_callbacks_reject_uninitialized_and_failed);
    RUN_TEST(capacity_success_decodes_payload);
    RUN_TEST(read_success_copies_block);
    RUN_TEST(write_success_records_request);
    RUN_TEST(tag_wraps_after_submit);
    RUN_TEST(partial_frame_waits_once_and_predicate_is_pure);
    RUN_TEST(split_header_then_payload_waits_twice);
    RUN_TEST(ring_wrap_header_is_read_correctly);
    RUN_TEST(response_ready_classifies_headers);
    RUN_TEST(capacity_bad_shape_header_faults_without_waiting);
    RUN_TEST(read_success_count_two_on_large_ring_is_rejected_without_waiting);
    RUN_TEST(non_read_request_consumes_complete_read_frame_then_protocol);
    RUN_TEST(pending_write_consumes_complete_capacity_frame_then_protocol);
    RUN_TEST(read_device_error_is_io_without_waiting);
    RUN_TEST(status_one_and_three_fault);
    RUN_TEST(wrong_tag_full_frame_is_consumed_then_rejected);
    RUN_TEST(wrong_lba_count_and_op_are_rejected);
    RUN_TEST(malformed_headers_fault);
    RUN_TEST(device_error_with_count_is_protocol);
    RUN_TEST(submit_again_or_invalid_faults_without_waiting);
    RUN_TEST(protocol_failure_stops_further_io);
    RUN_TEST(busy_reentry_is_protocol);
    RUN_TEST(per_request_configuration_change_faults);
    return UNITY_END();
}
