/* Host drive for the Guest block-protocol self-check,
 * tests/guest/block_check.c, against the real CPU, Bus and transport device in
 * src/ and the real block backend in tools/host_block.c.
 *
 * It is the counterpart of tests/guest/run_console.sh's scripted terminal
 * runner: the Guest images of docs/specs/0018-block-protocol.md have to be fed
 * channel states that tools/yan_run.c cannot produce, because a working host
 * only ever publishes whole responses:
 *
 *   --split        publishes one response in pieces, exactly as 0014 allows the
 *                  host to advance H2G_HEAD any number of times. Between pieces
 *                  the drive asserts that the Guest consumed nothing, which is
 *                  0018's rule 4 seen from the Host side.
 *   --ring-offset  rotates the response ring so a frame crosses the wrap point;
 *                  --request-offset does the same for a request header.
 *   --inject       answers the first request with a frame a working host would
 *                  never send: a non-zero reserved field with its payload, or a
 *                  count whose product with the block size does not fit the
 *                  ring.
 *   --serve none   never answers at all, so the Guest's own ring state is what
 *                  the flow-control checks see.
 *   --fail-before  injects backend faults on chosen requests.
 *
 * The Guest reports what it observed in the yan_block_check_* globals
 * (tests/guest/block_check.c); the drive reads them out of RAM and checks them
 * against what it actually did, so "the Guest saw a partial frame and consumed
 * nothing" is asserted rather than assumed.
 *
 * Detection criterion: a run counts as a failed check only when an assertion
 * printed ":FAIL:" and the run got as far as a verdict. A run that ends in the
 * step limit reports no assertion at all, so a hang can never be mistaken for a
 * detection - that is why the report checks are only evaluated once the Guest
 * reached `tohost`, and why exit 4 is distinct from 1 and 6.
 *
 * Exit codes match tools/yan_run.c: 0 tohost PASS, 2 usage, 4 no termination,
 * 5 Host error, 6 the Guest reported a failing check; 1 is added for a failed
 * Host assertion, printed with the same ":FAIL:" marker. */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_block.h"
#include "host_file.h"
#include "yan/cpu.h"
#include "yan/image.h"
#include "yan/machine.h"

enum {
    EXIT_PASS = 0,
    EXIT_HOST_ASSERTION = 1,
    EXIT_USAGE = 2,
    EXIT_NO_TERMINATION = 4,
    EXIT_HOST_ERROR = 5,
    /* The Guest reported a failure code *and* printed the marker its check
     * failures carry, so a check really failed. */
    EXIT_GUEST_FAILURE = 6,
    /* The Guest trapped, or ended on a code no check produces. Distinct from
     * EXIT_GUEST_FAILURE on purpose: a trap must never be read as "a check
     * failed", because only the second is evidence that a test has detection
     * power. */
    EXIT_GUEST_TRAP = 8
};

/* tests/guest/mtrap.h: the Guest panic handler ends the run with
 * YAN_GUEST_TRAP_PANIC | cause, so a trap is recognisable by its top bits. */
#define GUEST_TRAP_PANIC UINT32_C(0xbad0)
#define GUEST_TRAP_MASK UINT32_C(0xffffff80)

/* The geometry tools/yan_run.c uses for --disk, restated so a drift between the
 * two is a failure here instead of a protocol mystery. */
#define RING_SIZE UINT32_C(8192)
#define RING_BYTES (2U * RING_SIZE)
#define RAM_MARGIN (UINT32_C(64) * 1024U)
#define MAX_PIECES 16
#define MAX_FAILS 32

static unsigned failures;

/* The Host's own marker carries a [host] prefix so it cannot be mistaken for
 * one of the Guest's check failures: only the second is evidence that a Guest
 * check ran and failed, which is what the trap-cascade rule turns on. */
static void fail(const char *what)
{
    printf(":FAIL: [host] %s\n", what);
    ++failures;
}

static void expect(int condition, const char *what)
{
    if (!condition) {
        fail(what);
    }
}

static void expect_u32(uint32_t actual, uint32_t expected, const char *what)
{
    if (actual != expected) {
        printf(":FAIL: [host] %s: expected %" PRIu32 ", got %" PRIu32 "\n", what,
               expected, actual);
        ++failures;
    }
}

/* One byte of block `lba` at `offset`; see tests/guest/block_check.c. */
static uint8_t pattern(uint32_t lba, uint32_t offset)
{
    return (uint8_t)((lba * 37U + offset * 11U + (offset >> 8) * 5U) & 0xffU);
}

typedef struct {
    YanMachine machine;
    YanHostBlock block;
    /* Set when the Guest's byte stream has carried the ":FAIL:" marker its
     * check failures print. The marker is the only thing that separates "a
     * check failed" from "the run stopped for another reason". */
    int guest_marker;
    unsigned marker_at;
    uint8_t *storage;
    uint64_t blocks;
    uint32_t base;
    uint32_t ram_size;
    uint32_t ring_base;
    FILE *capture;
    int doorbell;
    int disk;
    int no_terminal;

    /* Scripted response shaping. */
    int serve;
    int split;
    uint32_t pieces[MAX_PIECES];
    uint32_t piece_count;
    uint32_t piece_steps;
    uint32_t piece_delay;
    uint32_t piece_at;
    uint32_t piece_remaining;
    uint32_t split_expect_head;
    uint32_t split_head;
    uint32_t split_tail;
    int split_active;
    uint32_t ring_offset;
    uint32_t request_offset;
    /* Write stale bytes over the part of the response header the host has not
     * published yet, so a receiver that reads a header it has not been given
     * decodes whatever the ring used to hold. */
    int stale_head;
    uint8_t stale_saved[16];
    int stale_planted;
    uint32_t fail_before[MAX_FAILS];
    uint32_t fail_count;
    int inject;
    int injected;
    uint32_t inject_length;
    uint32_t h2g_tail_start;

    /* What the script promises about the run. */
    int expect_wrapped;
    int expects_wrapped_set;
    uint32_t expect_g2h_bytes;
    int expects_g2h_set;
    uint32_t expect_consumed;
    int expects_consumed_set;
    uint32_t expect_served;
    int expects_served_set;
    uint32_t expect_pattern[8];
    uint32_t expect_pattern_count;
    uint32_t expect_written[8];
    uint32_t expect_written_count;
} Drive;

static void notify(void *context)
{
    *(int *)context = 1;
}

static bool terminal_tx_ready(void *context)
{
    (void)context;
    return true;
}

#define GUEST_MARKER ":FAIL:"

static void terminal_tx_write(void *context, uint8_t byte)
{
    Drive *drive = context;
    fputc(byte, stdout);
    if (drive->capture != NULL) {
        fputc(byte, drive->capture);
    }
    /* A rolling match, so the marker counts even when it is split across two
     * bytes of Guest output. */
    if (byte == (uint8_t)GUEST_MARKER[drive->marker_at]) {
        ++drive->marker_at;
        if (GUEST_MARKER[drive->marker_at] == '\0') {
            drive->guest_marker = 1;
            drive->marker_at = 0;
        }
    } else {
        drive->marker_at = byte == (uint8_t)GUEST_MARKER[0] ? 1U : 0U;
    }
}

static int number(const char *text, uint64_t *value)
{
    char *end = NULL;
    *value = strtoull(text, &end, 0);
    return end != text && *end == '\0';
}

/* A comma-separated list of decimal or hex values. `allow_zero` is for the
 * lists where zero is a value (a block number) and not a missing entry. */
static int parse_list(const char *text, uint32_t *out, uint32_t limit,
                      uint32_t *count, int allow_zero)
{
    *count = 0;
    const char *at = text;
    for (;;) {
        char *end = NULL;
        const uint64_t value = strtoull(at, &end, 0);
        if (end == at || (value == 0 && !allow_zero) || value > UINT32_MAX ||
            *count >= limit) {
            return 0;
        }
        out[(*count)++] = (uint32_t)value;
        if (*end == '\0') {
            return 1;
        }
        if (*end != ',') {
            return 0;
        }
        at = end + 1;
    }
}

static const char *usage_text =
    "usage: block_drive --image FILE [--base ADDR] [--ram BYTES]\n"
    "                   [--blocks N] [--max-steps N] [--capture FILE]\n"
    "                   [--serve MODE] [--split LIST] [--piece-steps N]\n"
    "                   [--ring-offset N] [--request-offset N] [--stale-head]\n"
    "                   [--fail-before LIST] [--inject MODE] [--no-terminal]\n"
    "                   [--expect-wrapped 0|1] [--expect-g2h-bytes N]\n"
    "                   [--expect-consumed N] [--expect-served N]\n"
    "                   [--expect-pattern LBA] [--expect-written LBA]\n"
    "  --serve auto|none             answer requests, or never answer\n"
    "  --split LIST                  publish each response in these sizes\n"
    "  --inject none|reserved|count   answer the first request with a frame\n"
    "                                that breaks the header rules\n";

/* --------------------------------------------------------------- ring state */

static const uint8_t *ring_pointer(const Drive *drive, uint32_t ring)
{
    return drive->machine.ram.data + (drive->ring_base - drive->base) +
           ring * RING_SIZE;
}

static void ring_write(const Drive *drive, uint32_t ring, uint32_t index,
                       const uint8_t *bytes, uint32_t count)
{
    uint8_t *target = (uint8_t *)ring_pointer(drive, ring);
    for (uint32_t i = 0; i < count; ++i) {
        target[(index + i) & (RING_SIZE - 1U)] = bytes[i];
    }
}

static void ring_read(const Drive *drive, uint32_t ring, uint32_t index,
                      uint8_t *out, uint32_t count)
{
    const uint8_t *source = ring_pointer(drive, ring);
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = source[(index + i) & (RING_SIZE - 1U)];
    }
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

/* The bytes a Guest-side check leaves in RAM. The names are the report globals
 * of tests/guest/block_check.c. */
typedef struct {
    uint32_t polls;
    uint32_t partial_seen;
    uint32_t partial_payload_seen;
    uint32_t tail_moved;
    uint32_t output_touched;
    uint32_t frame_wrapped;
    uint32_t checks;
} GuestReport;

static int read_guest_u32(const Drive *drive, const uint8_t *image,
                          size_t image_size, const char *name, uint32_t *value)
{
    uint32_t address = 0;
    if (yan_image_find_symbol(image, image_size, name, &address) != YAN_OK) {
        return 0;
    }
    if (address < drive->base || address - drive->base + 4 > drive->machine.ram.size) {
        return 0;
    }
    const uint8_t *at = drive->machine.ram.data + (address - drive->base);
    *value = (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
             ((uint32_t)at[3] << 24);
    return 1;
}

static int read_report(const Drive *drive, const uint8_t *image, size_t image_size,
                       GuestReport *report)
{
    memset(report, 0, sizeof *report);
    return read_guest_u32(drive, image, image_size, "yan_block_check_polls",
                          &report->polls) &&
           read_guest_u32(drive, image, image_size, "yan_block_check_partial_seen",
                          &report->partial_seen) &&
           read_guest_u32(drive, image, image_size,
                          "yan_block_check_partial_payload_seen",
                          &report->partial_payload_seen) &&
           read_guest_u32(drive, image, image_size, "yan_block_check_tail_moved",
                          &report->tail_moved) &&
           read_guest_u32(drive, image, image_size, "yan_block_check_output_touched",
                          &report->output_touched) &&
           read_guest_u32(drive, image, image_size, "yan_block_check_frame_wrapped",
                          &report->frame_wrapped) &&
           read_guest_u32(drive, image, image_size, "yan_block_check_checks",
                          &report->checks);
}

/* ------------------------------------------------------------ response work */

/* Answers the first request with a frame a working host never sends. */
static void inject_frame(Drive *drive)
{
    YanTransport *transport = &drive->machine.transport;
    if (yan_transport_host_readable(transport) < 16) {
        return;
    }
    uint8_t header[16];
    ring_read(drive, 0, transport->g2h_tail, header, 16);
    /* The Guest scenarios that use this submit exactly one read first. */
    expect(header[0] == 1 && header[1] == 0 && header[8] == 1,
           "the injected scenario submits one read first");
    expect(yan_transport_host_consume(transport, 16) == YAN_OK,
           "the request the crafted frame replaces is consumed");

    uint8_t frame[16 + 4096];
    memset(frame, 0, sizeof frame);
    frame[0] = 1; /* the op the Guest asked for */
    frame[1] = 0; /* a status that claims success */
    if (drive->inject == 1) {
        /* A non-zero reserved field with a complete payload behind it: a
         * receiver that does not consume by frame length loses the stream. */
        put_le16(frame + 2, 0x5a5a);
        put_le32(frame + 8, 1);
        put_le32(frame + 12, 1);
        for (uint32_t at = 0; at < 4096; ++at) {
            frame[16 + at] = (uint8_t)(0x5aU ^ (at & 0xffU));
        }
        drive->inject_length = 16 + 4096;
    } else {
        /* A count whose product with the block size runs past the ring: no such
         * frame can ever be complete, and waiting for it would hang. Only the
         * header exists, so only the header is consumed. */
        put_le16(frame + 2, 0x5b5b);
        put_le32(frame + 8, UINT32_C(0x00100001));
        drive->inject_length = 16;
    }
    ring_write(drive, 1, transport->h2g_head, frame, drive->inject_length);
    expect(yan_transport_host_publish(transport, drive->inject_length) == YAN_OK,
           "the crafted frame is published");
    drive->injected = 1;
}

/* Takes back the bytes the pump just published, so they can be published again
 * in pieces: same bytes, different publishing. */
static void queue_pieces(Drive *drive, uint32_t head_before, uint32_t answered)
{
    YanTransport *transport = &drive->machine.transport;
    const uint32_t head_after = transport->h2g_head;
    const uint32_t total = (head_after - head_before) & (RING_SIZE - 1U);
    expect(answered <= 1, "one request is in flight at a time in this scenario");

    uint32_t sum = 0;
    for (uint32_t i = 0; i < drive->piece_count; ++i) {
        sum += drive->pieces[i];
    }
    if (sum > total) {
        printf(":FAIL: [host] the piece sizes (%" PRIu32
               ") exceed the response (%" PRIu32 ")\n", sum, total);
        ++failures;
        return;
    }
    /* Whatever the list does not name goes out as one last piece, so the whole
     * response is always published. */
    if (sum < total) {
        if (drive->piece_count >= MAX_PIECES) {
            fail("too many pieces");
            return;
        }
        drive->pieces[drive->piece_count++] = total - sum;
    }
    transport->h2g_head = head_before;
    /* 0018 rule 1 from the hostile side: the host has published only the first
     * piece, and everything behind it is whatever the ring held before. A
     * receiver that reads those bytes anyway is reading a header nobody sent,
     * so the bytes are chosen to decode as a count no ring could hold. */
    if (drive->stale_head && !drive->stale_planted) {
        /* op and status are the first piece, so only the bytes behind it are
         * planted: the count becomes 0x00100001, whose product with the block
         * size does not fit any ring. */
        static const uint8_t planted[16] = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0x10, 0, 0, 0, 0, 0};
        uint8_t scratch[16];
        ring_read(drive, 1, head_before, scratch, sizeof scratch);
        for (uint32_t i = 0; i < sizeof drive->stale_saved; ++i) {
            drive->stale_saved[i] = scratch[i];
        }
        for (uint32_t i = 1; i < sizeof scratch; ++i) {
            scratch[i] = planted[i];
        }
        ring_write(drive, 1, head_before, scratch, sizeof scratch);
        drive->stale_planted = 1;
    }
    drive->split_active = 1;
    drive->piece_at = 0;
    drive->piece_remaining = total;
    drive->piece_delay = 0;
    drive->split_expect_head = head_after;
    drive->split_head = head_before;
    drive->split_tail = transport->h2g_tail;
}

/* The middle of a response: the line is asserted, the frame is not complete,
 * and the Guest must not have taken any of it. */
static void publish_pieces(Drive *drive)
{
    YanTransport *transport = &drive->machine.transport;
    if (!drive->split_active) {
        return;
    }
    if (drive->piece_delay > 0) {
        --drive->piece_delay;
        return;
    }
    if (transport->h2g_tail != drive->split_tail) {
        expect_u32(transport->h2g_tail, drive->split_tail,
                   "the Guest consumed nothing while the frame was incomplete");
    }
    uint32_t piece = drive->pieces[drive->piece_at];
    if (piece > drive->piece_remaining) {
        piece = drive->piece_remaining;
    }
    /* The first piece has been published and observed; the rest of the frame is
     * written back before the host publishes it, which is all a real host does
     * (the bytes in the ring are the host's to write before it moves the head). */
    if (drive->stale_planted && drive->piece_at > 0) {
        ring_write(drive, 1, drive->split_head, drive->stale_saved,
                   sizeof drive->stale_saved);
        drive->stale_planted = 0;
    }
    expect(yan_transport_host_publish(transport, piece) == YAN_OK,
           "a piece of the response is published");
    drive->piece_remaining -= piece;
    ++drive->piece_at;
    drive->piece_delay = drive->piece_steps;
    if (drive->piece_remaining == 0) {
        drive->split_active = 0;
        expect_u32(transport->h2g_head, drive->split_expect_head,
                   "the pieces add up to the response the pump produced");
    }
}

/* The next request number the backend should fail on, if any. */
static int fail_this_request(const Drive *drive)
{
    for (uint32_t i = 0; i < drive->fail_count; ++i) {
        if (drive->fail_before[i] == drive->block.served + 1) {
            return 1;
        }
    }
    return 0;
}

static void service_once(Drive *drive)
{
    YanTransport *transport = &drive->machine.transport;
    if (!drive->disk) {
        return;
    }
    if (drive->inject != 0 && !drive->injected) {
        inject_frame(drive);
        return;
    }
    if (yan_transport_host_readable(transport) < 16) {
        return;
    }
    if (fail_this_request(drive)) {
        drive->block.fail_next = true;
    }
    const uint32_t head_before = transport->h2g_head;
    const uint32_t answered =
        yan_host_block_service(&drive->block, transport, &drive->machine.ram,
                               drive->base);
    if (answered == 0) {
        return;
    }
    if (drive->split) {
        queue_pieces(drive, head_before, answered);
    }
}

/* --------------------------------------------------------------- the checks */

static int storage_matches(const Drive *drive, uint32_t lba)
{
    if (drive->storage == NULL || lba >= drive->blocks) {
        return 0;
    }
    for (uint32_t at = 0; at < YAN_HOST_BLOCK_BLOCK_SIZE; ++at) {
        if (drive->storage[(size_t)lba * YAN_HOST_BLOCK_BLOCK_SIZE + at] !=
            pattern(lba, at)) {
            return 0;
        }
    }
    return 1;
}

/* The payload the Guest writes when it wants the drive to be able to tell a
 * write that landed from a block that already held its content: the drive's own
 * pattern, inverted. tests/guest/block_check.c writes exactly these bytes, so
 * the comparison is byte for byte and not "something changed". */
static int storage_written(const Drive *drive, uint32_t lba)
{
    if (drive->storage == NULL || lba >= drive->blocks) {
        return 0;
    }
    for (uint32_t at = 0; at < YAN_HOST_BLOCK_BLOCK_SIZE; ++at) {
        if (drive->storage[(size_t)lba * YAN_HOST_BLOCK_BLOCK_SIZE + at] !=
            (uint8_t)(pattern(lba, at) ^ 0xffU)) {
            return 0;
        }
    }
    return 1;
}

/* Everything the script promised about this run, and everything the Host can
 * see about the Guest's framing. Only called once the Guest reached a verdict. */
static void check_run(Drive *drive, const uint8_t *image, size_t image_size)
{
    const YanTransport *transport = &drive->machine.transport;
    if (drive->expects_g2h_set) {
        expect_u32(transport->g2h_head & (RING_SIZE - 1U), drive->expect_g2h_bytes,
                   "the guest-to-host ring holds exactly the expected bytes");
    }
    if (drive->expects_served_set) {
        expect_u32((uint32_t)drive->block.served, drive->expect_served,
                   "the backend answered the expected number of requests");
    }
    if (drive->expects_consumed_set) {
        const uint32_t consumed =
            (transport->h2g_tail - drive->h2g_tail_start) & (RING_SIZE - 1U);
        expect_u32(consumed, drive->expect_consumed,
                   "the guest consumed exactly the frames it was sent");
    }
    for (uint32_t i = 0; i < drive->expect_pattern_count; ++i) {
        expect(storage_matches(drive, drive->expect_pattern[i]),
               "the block still holds its original content");
    }
    for (uint32_t i = 0; i < drive->expect_written_count; ++i) {
        expect(storage_written(drive, drive->expect_written[i]),
               "the block holds exactly what the guest wrote");
    }

    GuestReport report;
    if (!read_report(drive, image, image_size, &report)) {
        fail("the guest report globals could not be read");
        return;
    }
    expect(report.checks > 0, "the guest ran its checks");
    expect_u32(report.tail_moved, 0,
               "the guest never moved its tail while a frame was incomplete");
    expect_u32(report.output_touched, 0,
               "the guest never handed a half frame to its caller");
    if (drive->split) {
        expect(report.polls >= 8, "the guest polled while the response was split");
        /* What the guest had to be able to see is a function of the pieces the
         * drive published: a first piece below the header size is a partial
         * header, and any boundary past a whole header is a missing payload. */
        uint32_t seen = 0;
        int partial_header = drive->piece_count > 0 && drive->pieces[0] < 16;
        int partial_payload = 0;
        for (uint32_t i = 0; i < drive->piece_count; ++i) {
            seen += drive->pieces[i];
            if (seen >= 16 && seen < 16 + 4096) {
                partial_payload = 1;
            }
        }
        if (partial_header) {
            expect_u32(report.partial_seen, 1, "the guest observed a partial header");
        }
        if (partial_payload) {
            expect_u32(report.partial_payload_seen, 1,
                       "the guest observed a response whose payload was missing");
        }
    }
    if (drive->expects_wrapped_set) {
        expect_u32(report.frame_wrapped, (uint32_t)drive->expect_wrapped,
                   "the frame crossed the ring wrap point as intended");
    }
}

/* --------------------------------------------------------------- arguments */

static int parse_options(int argc, char **argv, Drive *drive, const char **image,
                         uint64_t *max_steps, const char **capture)
{
    *image = NULL;
    *max_steps = UINT64_C(1000000);
    *capture = NULL;
    drive->serve = 1;
    drive->piece_steps = 2000;
    drive->base = UINT32_C(0x80000000);
    drive->ram_size = 16U * 1024U * 1024U;

    for (int i = 1; i < argc; ++i) {
        uint64_t value = 0;
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            *image = argv[++i];
        } else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            *capture = argv[++i];
        } else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &drive->blocks)) return 0;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], max_steps)) return 0;
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->base = (uint32_t)value;
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->ram_size = (uint32_t)value;
        } else if (strcmp(argv[i], "--piece-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->piece_steps = (uint32_t)value;
        } else if (strcmp(argv[i], "--ring-offset") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->ring_offset = (uint32_t)value;
        } else if (strcmp(argv[i], "--request-offset") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->request_offset = (uint32_t)value;
        } else if (strcmp(argv[i], "--stale-head") == 0) {
            drive->stale_head = 1;
        } else if (strcmp(argv[i], "--expect-g2h-bytes") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->expect_g2h_bytes = (uint32_t)value;
            drive->expects_g2h_set = 1;
        } else if (strcmp(argv[i], "--expect-consumed") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->expect_consumed = (uint32_t)value;
            drive->expects_consumed_set = 1;
        } else if (strcmp(argv[i], "--expect-served") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            drive->expect_served = (uint32_t)value;
            drive->expects_served_set = 1;
        } else if (strcmp(argv[i], "--expect-wrapped") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value) || value > 1) return 0;
            drive->expect_wrapped = (int)value;
            drive->expects_wrapped_set = 1;
        } else if (strcmp(argv[i], "--expect-pattern") == 0 && i + 1 < argc) {
            if (!parse_list(argv[++i], drive->expect_pattern, 8,
                            &drive->expect_pattern_count, 1)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--expect-written") == 0 && i + 1 < argc) {
            if (!parse_list(argv[++i], drive->expect_written, 8,
                            &drive->expect_written_count, 1)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "auto") == 0) drive->serve = 1;
            else if (strcmp(mode, "none") == 0) drive->serve = 0;
            else return 0;
        } else if (strcmp(argv[i], "--inject") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "reserved") == 0) drive->inject = 1;
            else if (strcmp(mode, "count") == 0) drive->inject = 2;
            else return 0;
        } else if (strcmp(argv[i], "--split") == 0 && i + 1 < argc) {
            if (!parse_list(argv[++i], drive->pieces, MAX_PIECES,
                            &drive->piece_count, 0)) {
                return 0;
            }
            drive->split = 1;
        } else if (strcmp(argv[i], "--fail-before") == 0 && i + 1 < argc) {
            if (!parse_list(argv[++i], drive->fail_before, MAX_FAILS,
                            &drive->fail_count, 0)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--no-terminal") == 0) {
            drive->no_terminal = 1;
        } else {
            return 0;
        }
    }
    return *image != NULL && drive->ram_size >= RING_BYTES + RAM_MARGIN;
}

int main(int argc, char **argv)
{
    Drive drive;
    memset(&drive, 0, sizeof drive);
    const char *image_path = NULL;
    const char *capture_path = NULL;
    uint64_t max_steps = 0;
    uint8_t *image = NULL;
    size_t image_size = 0;
    YanImageInfo info = {0};
    int result = EXIT_HOST_ERROR;
    int stopped = 0;
    uint32_t tohost = 0;
    int tohost_known = 0;
    uint32_t guest_code = 0;

    if (!parse_options(argc, argv, &drive, &image_path, &max_steps, &capture_path)) {
        fputs(usage_text, stderr);
        return EXIT_USAGE;
    }
    if (yan_machine_init_with(&drive.machine, drive.base, drive.ram_size) != YAN_OK) {
        fprintf(stderr, "block_drive: cannot build the machine\n");
        return EXIT_HOST_ERROR;
    }
    drive.ring_base = drive.base + drive.ram_size - RING_BYTES;
    if (capture_path != NULL) {
        drive.capture = fopen(capture_path, "wb");
        if (drive.capture == NULL) {
            fprintf(stderr, "block_drive: cannot write '%s'\n", capture_path);
            goto done;
        }
    }
    if (drive.blocks != 0) {
        if (yan_transport_configure(&drive.machine.transport, drive.ring_base,
                                    RING_SIZE, drive.base,
                                    drive.ram_size) != YAN_OK) {
            fprintf(stderr, "block_drive: cannot place the channel rings\n");
            goto done;
        }
        yan_transport_set_notify(&drive.machine.transport, notify, &drive.doorbell);
        drive.storage = calloc((size_t)drive.blocks, YAN_HOST_BLOCK_BLOCK_SIZE);
        if (drive.storage == NULL) {
            fprintf(stderr, "block_drive: cannot allocate the backing store\n");
            goto done;
        }
        for (uint32_t lba = 0; lba < drive.blocks; ++lba) {
            for (uint32_t at = 0; at < YAN_HOST_BLOCK_BLOCK_SIZE; ++at) {
                drive.storage[(size_t)lba * YAN_HOST_BLOCK_BLOCK_SIZE + at] =
                    pattern(lba, at);
            }
        }
        if (yan_host_block_init(&drive.block, drive.storage, drive.blocks) != YAN_OK) {
            fprintf(stderr, "block_drive: cannot initialise the block device\n");
            goto done;
        }
        drive.disk = 1;
        /* A ring position other than zero is what a channel that has already
         * carried traffic looks like. The protocol must not care, and it is
         * what makes a frame cross the wrap point. */
        drive.machine.transport.g2h_head = drive.request_offset & (RING_SIZE - 1U);
        drive.machine.transport.g2h_tail = drive.machine.transport.g2h_head;
        drive.machine.transport.h2g_head = drive.ring_offset & (RING_SIZE - 1U);
        drive.machine.transport.h2g_tail = drive.machine.transport.h2g_head;
    }
    drive.h2g_tail_start = drive.machine.transport.h2g_tail;

    if (!drive.no_terminal) {
        const YanUartTerminal terminal = {.context = &drive,
                                          .tx_ready = terminal_tx_ready,
                                          .tx_write = terminal_tx_write};
        if (yan_uart_set_terminal(&drive.machine.uart, &terminal) != YAN_OK) {
            fprintf(stderr, "block_drive: cannot attach the capture backend\n");
            goto done;
        }
    }
    image = yan_host_read_file(image_path, &image_size);
    if (image == NULL) {
        fprintf(stderr, "block_drive: cannot read '%s'\n", image_path);
        goto done;
    }
    if (yan_image_load_elf(&drive.machine.ram, drive.base, image, image_size,
                           &info) != YAN_OK) {
        fprintf(stderr, "block_drive: '%s' is not a loadable RV32 ELF image\n",
                image_path);
        goto done;
    }
    if (yan_cpu_reset(&drive.machine.cpu, info.entry) != YAN_OK) {
        goto done;
    }
    if (yan_image_find_symbol(image, image_size, "tohost", &tohost) == YAN_OK) {
        tohost_known = 1;
    }

    for (uint64_t step = 0; step < max_steps; ++step) {
        /* Served before the step, exactly as tools/yan_run.c does it: the step
         * that samples the device lines is the step that can deliver the
         * notification the publish asserted. */
        if (drive.doorbell) {
            drive.doorbell = 0;
            if (drive.serve) {
                service_once(&drive);
            }
        }
        publish_pieces(&drive);
        (void)yan_machine_step(&drive.machine);
        if (tohost_known) {
            uint32_t value = 0;
            if (yan_bus_read(&drive.machine.bus, tohost, 4, &value).status == YAN_OK &&
                value != 0) {
                guest_code = value;
                stopped = 1;
                break;
            }
        }
    }
    if (!stopped) {
        /* No verdict was reached, so nothing is asserted about this run: a
         * hang is reported as "no termination" and never as a failed check. */
        fprintf(stderr, "block_drive: stopped after %" PRIu64
                " steps without reaching tohost\n", max_steps);
        result = EXIT_NO_TERMINATION;
        goto done;
    }

    /* The host's promises about the run ("the backend answered N requests",
     * "the block holds what was written") describe a run that reached a verdict.
     * A trap or a stray tohost value is not that, and evaluating them anyway
     * would fill the log with :FAIL: lines that say nothing about the Guest's
     * checks - exactly the confusion the trap classification exists to avoid. */
    const int ended_in_a_check =
        guest_code == 1 ||
        ((guest_code & UINT32_C(0x80000000)) != 0 && drive.guest_marker);
    if (ended_in_a_check) {
        check_run(&drive, image, image_size);
    } else {
        printf("NOTE: the run did not end in a check, so this drive's expectations "
               "about it were not evaluated\n");
    }
    if (guest_code == 1) {
        if (failures != 0) {
            result = EXIT_HOST_ASSERTION;
        } else {
            printf("PASS the block protocol drive reached the guest's final check\n");
            result = EXIT_PASS;
        }
    } else if ((guest_code & GUEST_TRAP_MASK) ==
               (GUEST_TRAP_PANIC & GUEST_TRAP_MASK)) {
        /* The Guest's own panic handler: a trap, not a failed expectation. */
        printf("TRAP: the guest trapped; its panic handler wrote %08" PRIx32 "\n",
               guest_code);
        result = EXIT_GUEST_TRAP;
    } else if ((guest_code & UINT32_C(0x80000000)) != 0 && drive.guest_marker) {
        /* A check number with the marker the check failures print: this is the
         * Guest's own assertion, and the message above says which one. */
        printf("CHECK-FAIL: the guest reported failure code %" PRIu32
               " after printing its marker\n", guest_code);
        result = EXIT_GUEST_FAILURE;
    } else {
        printf("UNEXPECTED: the run ended with tohost = %08" PRIx32
               " and no failed check\n", guest_code);
        result = EXIT_GUEST_TRAP;
    }

done:
    if (drive.capture != NULL) {
        fclose(drive.capture);
    }
    free(image);
    free(drive.storage);
    yan_machine_destroy(&drive.machine);
    return result;
}
