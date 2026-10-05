/* Combined acceptance case: a cooperative-runtime task performing block I/O
 * through the block request protocol against the *file* backend
 * (`yan_run --disk-image FILE`), and a second simulator process reading the
 * same image back.
 *
 * What this image is for
 * ----------------------
 * docs/specs/0018-block-protocol.md (os/block.c, tools/host_block.c) and
 * docs/specs/0019-cooperative-runtime.md (os/task.c) were verified separately,
 * and the file backend (tools/host_disk.c) was verified on its own as well.
 * tests/guest/persistent_block_check.c writes through a *polling* loop and
 * tests/guest/m2a_combined.c exercises the runtime on `--disk`, a RAM device
 * that cannot persist anything. This image is the missing combination: the
 * request is issued by a task, awaited with yan_os_task_wait() rather than
 * polled, and the bytes it writes must survive the process.
 *
 * The same source is built twice with -DPERSIST_ROLE:
 *
 *   writer  (PERSIST_ROLE=0) issues the requests and exits through tohost once
 *           the awaited write has been answered;
 *   reader  (PERSIST_ROLE=1) is a *fresh process* on the same image and reads
 *           the written blocks back byte for byte.
 *
 * The host harness (tests/guest/run_persistent_combined.py) mounts the image
 * itself between the two runs and compares the target block, the neighbouring
 * blocks and the length independently of both guests.
 *
 * The fast path and the blocked path
 * ----------------------------------
 * 0019's wait has two exits and both are covered here, separately:
 *
 *   fast     the reply is already in the ring when wait() is called. The
 *            predicate is asked once inside the critical section, no task is
 *            registered and the caller keeps running - so the other task must
 *            not have made any progress across that call. The image asserts
 *            exactly that (polls delta 1, peer-progress delta 0).
 *   blocked  the reply cannot exist yet. This image builds a real BLOCKED
 *            window out of the response ring's flow control (0018), not out of
 *            host latency:
 *
 *              1. one READ of block 2 is submitted. Its 4112-byte response is
 *                 published and stays in the ring: it is the first frame the
 *                 *other* task drains at the end of the window;
 *              2. PC_FILL_FRAMES capacity queries are submitted, then a filler
 *                 WRITE of block 1. Together with the read response they leave
 *                 the ring with 7 free bytes, so the 16-byte reply the awaited
 *                 WRITE needs no longer fits and host_block.c declines to serve
 *                 it - it stays in the request ring;
 *              3. the I/O task arms the window and calls wait() for the tag of
 *                 that write. The predicate asks for its own frame, the ring
 *                 head is the read response, so it answers no and the task is
 *                 marked BLOCKED;
 *              4. the other task spends the whole window yielding (every tick
 *                 therefore happens while the I/O task is BLOCKED), then drains
 *                 the PC_FRAMES_BEFORE_TARGET frames that are ahead of the
 *                 awaited reply - the read response, the 169 capacity responses
 *                 and the filler write response, each one checked. That is what
 *                 frees the ring;
 *              5. the pump answers the queued write on its own at the next
 *                 instruction boundary. Publishing the reply asserts the
 *                 channel line, the PLIC turns it into MEIP, and
 *                 os/trap_entry.S's handler services the device and wakes the
 *                 waiter. The other task drains without yielding, so the woken
 *                 task does not run until the awaited reply is the only frame
 *                 left;
 *              6. the I/O task takes that one frame and checks the write's
 *                 response.
 *
 * The ring arithmetic is exact and asserted, not hoped for:
 * 4112 + 169*24 + 16 = 8184 used, 7 free, less than the 16 the write's reply
 * needs; after the other task's 171 takes everything the queue held has been
 * served, and the awaited reply is the single frame left. That is why the frame
 * counts are PC_FRAMES_BEFORE_TARGET and 1, and not "some frames".
 *
 * Leaving the awaited reply at the head is also what keeps a faked wait honest:
 * a runtime that replaces wait with "yield and ask the predicate again" only
 * returns once that head is the awaited frame, and by then it has asked many
 * times - so the window's poll count, not the write's success, separates the
 * two.
 *
 * A second window follows with the shape the M2a case already uses (two reads,
 * the second one denied because two 4112-byte responses cannot fit), and its
 * drained frame is the read-back of the block this task wrote - so the same
 * process proves the write reached the file backend before it exits. Its budget
 * (32) differs from the first window's (64) so the waiter's own activity
 * provably does not scale with the other task's progress.
 *
 * The discriminating assertion
 * ----------------------------
 * "The write succeeded" does not separate a real wait from the yield+busy-wait
 * 0019 forbids: the response arrives either way. What separates them is whether
 * the waiting task is *scheduled* while it waits. The measured quantities are
 *
 *   ticks  how many rounds the other task completed inside the window; every
 *          one of them happened while this task was BLOCKED (the armed store
 *          sits immediately before the blocking wait, with no yield between);
 *   polls  how many times the runtime asked this task's predicate. A
 *          block-and-wake runtime asks exactly once and never again; a
 *          yield+busy-wait runtime asks once per round, so polls tracks ticks.
 *
 * The predicate's contract (os/task.h) is "short, read-only, no side effects":
 * it reads the ring's positions and the 16-byte header at its head, the same
 * peek os/block.c uses, and nothing else. The one extra store is the trace
 * counter this file reads back, taken after the answer has been computed and
 * never read by the runtime.
 *
 * Verdicts (0013): tohost 1 means the case passed; anything else is a failure
 * code with the top bit set. This case's codes carry 0x50000000 so a host can
 * tell them from the M2a case's 0x40000000 and from the runtime's panic codes.
 *
 * Build and run: tests/guest/run_persistent_combined.py. The production path is
 * `yan_run --image ... --disk-image FILE --terminal`; the harness builds this
 * file twice and runs the two roles in two processes. */

#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "console.h"
#include "guest.h"
#include "platform.h"
#include "task.h"

#define PC_ROLE_WRITER 0
#define PC_ROLE_READER 1

#ifndef PERSIST_ROLE
#define PERSIST_ROLE PC_ROLE_WRITER
#endif
#ifndef PERSIST_DISK_BLOCKS
#define PERSIST_DISK_BLOCKS 8u
#endif

/* ------------------------------------------------------------- constants */

#define PC_BLOCK_BYTES ((uint32_t)YAN_OS_BLOCK_BLOCK_SIZE)
#define PC_HEADER_BYTES ((uint32_t)YAN_OS_BLOCK_HEADER_SIZE)
/* A one-block read response, which is also the largest frame 0018's ring floor
 * (RING_SIZE 8192) carries: two of them do not fit, which is the flow control
 * the second window is built on. */
#define PC_READ_FRAME (PC_HEADER_BYTES + PC_BLOCK_BYTES)
/* A capacity response carries the little-endian u64 block count. */
#define PC_CAPACITY_FRAME (PC_HEADER_BYTES + 8u)

/* The write window's geometry. 169 capacity responses plus the read response
 * plus the filler leave 7 free bytes, one byte short of what the awaited
 * write's 16-byte reply needs - that byte is the whole window. The other task
 * then takes the frames ahead of the reply and the I/O task takes the reply. */
#define PC_FILL_FRAMES 169u
#define PC_FRAMES_BEFORE_TARGET (PC_FILL_FRAMES + 2u)
#define PC_WINDOW_WRITE 64u
#define PC_WINDOW_READ 32u
#define PC_ROUNDS 2u

/* The blocks this case owns. Everything else in the image is left as the
 * harness seeded it, and both the guest and the host compare it that way. */
#define PC_LBA_TARGET 1u
#define PC_LBA_NEIGHBOR 2u
#define PC_LBA_FAST 3u

/* Tags pair a response with the request it answers (0018). The capacity tags
 * are a contiguous range so the drain can recognise them in order. */
#define PC_TAG_CAPACITY 0x0101u
#define PC_TAG_FAST 0x0110u
#define PC_TAG_READ_SEED 0x0201u
#define PC_TAG_FILL 0x0202u
#define PC_TAG_TARGET 0x0203u
#define PC_TAG_READ_FIRST 0x0211u
#define PC_TAG_READ_SECOND 0x0212u
#define PC_TAG_CAPACITY_BASE 0x3000u

/* Which content a drained frame must carry; PC_KIND_NONE is a frame that has
 * no payload to compare (a write or a capacity response). */
#define PC_KIND_SEED 0u
#define PC_KIND_PATTERN_A 1u
#define PC_KIND_NONE 2u

/* ------------------------------------------------------------ fail codes */

#define PC_FAIL(reason) (UINT32_C(0x50000000) | (reason))

#define PC_FAIL_NO_CHANNEL 1u          /* no host pump: run it with --disk-image */
#define PC_FAIL_MAX_COUNT 2u           /* the ring geometry is not 8192          */
#define PC_FAIL_SPAWN 3u               /* the runtime refused a task slot        */
#define PC_FAIL_CAPACITY_TAKE 4u       /* the capacity response was not taken    */
#define PC_FAIL_CAPACITY_FRAME 5u      /* its header did not match               */
#define PC_FAIL_CAPACITY_VALUE 6u      /* the device reported another size       */
#define PC_FAIL_PATTERN_WEAK 7u        /* the test's own patterns prove nothing  */
#define PC_FAIL_FAST_SUBMIT 8u         /* the fast-path write was not published  */
#define PC_FAIL_FAST_TAKE 9u           /* its response was not taken             */
#define PC_FAIL_FAST_FRAME 10u         /* its response did not match             */
#define PC_FAIL_FAST_NOT_FAST 11u      /* wait did not take the already-happened path */
#define PC_FAIL_FILL_SUBMIT 12u        /* a back-pressure request was not published */
#define PC_FAIL_BACKPRESSURE 13u       /* the awaited reply still fitted the ring  */
#define PC_FAIL_WINDOW_SUBMIT 14u      /* a window request was not published       */
#define PC_FAIL_DRAIN_MISSING 15u      /* the wake happened without the peer drain */
#define PC_FAIL_DRAIN_FRAME 16u        /* the peer's frame header was wrong        */
#define PC_FAIL_DRAIN_PAYLOAD 17u      /* the peer's frame carried other bytes     */
#define PC_FAIL_ISR_NOT_SERVICED 18u   /* the wake left the device asserted        */
#define PC_FAIL_NO_WINDOW 19u          /* wait returned before the window ran      */
#define PC_FAIL_POLLED_WHILE_BLOCKED 20u /* the waiter ran while it was blocked    */
#define PC_FAIL_WINDOW_TAKE 21u        /* the awaited frame was not delivered      */
#define PC_FAIL_WINDOW_FRAME 22u       /* the awaited frame did not match          */
#define PC_FAIL_FRAME_COUNT 23u        /* the drain did not take the frames ahead  */
#define PC_FAIL_RING_NOT_EMPTY 24u     /* a frame of the previous phase was left   */
#define PC_FAIL_PROGRESS 25u           /* the other task never finished            */
#define PC_FAIL_READ_SUBMIT 26u        /* a reader request was not published       */
#define PC_FAIL_READ_TAKE 27u          /* a reader response was not delivered      */
#define PC_FAIL_READ_FRAME 28u         /* a reader response did not match          */
#define PC_FAIL_READ_PAYLOAD 29u       /* a reader payload differed from the file  */

/* ------------------------------------------------------------ the console */

/* 0017's console never blocks and reports YAN_OS_UNAVAILABLE when no terminal
 * is attached, so a headless run still reaches its verdict with no
 * diagnostics. The harness runs with --terminal and greps these lines: an exit
 * 0 without them would only mean "the image finished". */
static void pc_puts(const char *text)
{
    (void)yan_os_console_puts(text);
}

static void pc_put_hex(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    (void)yan_os_console_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        (void)yan_os_console_putc(digits[(value >> (unsigned)shift) & 0xfu]);
    }
}

static void pc_put_dec(uint32_t value)
{
    char text[11];
    unsigned at = sizeof text;
    text[--at] = '\0';
    do {
        text[--at] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);
    (void)yan_os_console_puts(&text[at]);
}

static volatile uint32_t pc_phase = 0;

__attribute__((noreturn)) static void pc_fail(uint32_t reason)
{
    pc_puts("pcombined: FAIL code=");
    pc_put_hex(PC_FAIL(reason));
    pc_puts(" phase=");
    pc_put_dec(pc_phase);
    pc_puts("\r\n");
    guest_finish(PC_FAIL(reason));
    for (;;) {
    }
}

/* ------------------------------------------------------------- patterns */

/* The four byte patterns, as plain closed forms so the host harness can
 * reproduce every byte in a handful of lines of its own language instead of
 * reimplementing the guest (see run_persistent_combined.py). Every pattern is a
 * function of the byte's index *inside its 4096-byte block*: the harness seeds
 * each block with pc_seed_byte, and the writer overwrites block 1 (pattern A,
 * after the filler pattern B) and block 3 (pattern C). */
static uint8_t pc_seed_byte(uint32_t i)
{
    return (uint8_t)((i * 7u + (i >> 8) * 11u + 93u) & 255u);
}

static uint8_t pc_pattern_a_byte(uint32_t i)
{
    return (uint8_t)((i * 17u + (i >> 8) * 29u + 41u) & 255u);
}

static uint8_t pc_pattern_c_byte(uint32_t i)
{
    return (uint8_t)((i * 17u + (i >> 8) * 29u + 13u) & 255u);
}

typedef uint8_t (*PcByteFn)(uint32_t index);

static int pc_matches(const uint8_t *buffer, PcByteFn byte)
{
    for (uint32_t i = 0; i < PC_BLOCK_BYTES; ++i) {
        if (buffer[i] != byte(i)) {
            return 0;
        }
    }
    return 1;
}

/* The filler pattern is written and then overwritten by the target pattern in
 * the same window, so only the writer needs it - and only the writer checks
 * that the patterns are distinguishable at all. */
#if PERSIST_ROLE == PC_ROLE_WRITER
static uint8_t pc_pattern_b_byte(uint32_t i)
{
    return (uint8_t)((i * 17u + (i >> 8) * 29u + 7u) & 255u);
}

static void pc_fill(uint8_t *buffer, PcByteFn byte)
{
    for (uint32_t i = 0; i < PC_BLOCK_BYTES; ++i) {
        buffer[i] = byte(i);
    }
}

/* A payload comparison is only evidence if the payload could not have been
 * produced by accident: this asserts the pattern is not a constant block, so a
 * zero-filled response (or one repeated byte) can never match it. */
static int pc_is_strong(PcByteFn byte)
{
    uint8_t seen[256];
    uint32_t distinct = 0;
    memset(seen, 0, sizeof seen);
    for (uint32_t i = 0; i < PC_BLOCK_BYTES; ++i) {
        if (seen[byte(i)] == 0) {
            seen[byte(i)] = 1;
            ++distinct;
        }
    }
    return distinct >= 200u;
}

static int pc_differs(PcByteFn left, PcByteFn right)
{
    for (uint32_t i = 0; i < PC_BLOCK_BYTES; ++i) {
        if (left(i) != right(i)) {
            return 1;
        }
    }
    return 0;
}

static PcByteFn pc_kind_bytes(uint32_t kind)
{
    return kind == PC_KIND_SEED ? pc_seed_byte : pc_pattern_a_byte;
}
#endif

/* -------------------------------------------------------------- buffers */

/* Static, because a freestanding guest has no heap. The two tasks never share
 * one: the I/O task's buffer would otherwise be overwritten by the peer at the
 * moment the case is asserting its contents. */
static uint8_t pc_io_buffer[PC_BLOCK_BYTES];
#if PERSIST_ROLE == PC_ROLE_WRITER
static uint8_t pc_peer_buffer[PC_BLOCK_BYTES];
#endif

/* ---------------------------------------------------------------- shared */

/* Between the two tasks. volatile because one task reads what the other wrote;
 * there is a single hart, so the only thing that matters is that the compiler
 * does not cache a value across a yield. */
typedef struct {
    uint32_t polls;                   /* every predicate call, after the answer */
    uint32_t armed;                   /* round the I/O task has armed, 0 = none */
    uint32_t budget[PC_ROUNDS];       /* the tick budget the I/O task asked for */
    uint32_t ticks[PC_ROUNDS];        /* rounds the other task completed in it  */
    uint32_t woken;                   /* round whose awaited frame was taken    */
    uint32_t idle_ticks;              /* the other task's ticks outside a window */
    uint32_t done;                    /* the other task finished both windows   */
    uint32_t drain_tag[PC_ROUNDS];    /* the frame the other task takes first   */
    uint32_t drain_lba[PC_ROUNDS];
    uint32_t drain_kind[PC_ROUNDS];
    uint32_t drain_frames[PC_ROUNDS]; /* frames it took in that round           */
    uint32_t drain_fail[PC_ROUNDS];   /* 0 = took them and they were correct    */
} PcShared;

static volatile PcShared pc_shared;

/* ------------------------------------------------------------- predicate */

typedef struct {
    uint32_t expected; /* bytes a complete response frame occupies */
    uint32_t tag;      /* the tag of the response being waited for */
} PcWait;

/* The i-th byte of the frame at the head of the response ring, read without
 * consuming anything - the same shape as os/block.c's peek_response. */
static uint8_t pc_peek_response(uint32_t index)
{
    const uint32_t size = yan_os_transport_ring_size();
    const volatile uint8_t *ring =
        (const volatile uint8_t *)(uintptr_t)(yan_os_transport_ring_base() + size);
    return ring[yan_os_ring_index(yan_os_transport_h2g_tail(), size, index)];
}

/* The wait predicate.
 *
 * Two conditions, because "a complete response has arrived" is not the same as
 * "the response this task is waiting for has arrived": the write window
 * deliberately leaves earlier frames in the ring, and the tag 0018 keeps in the
 * protocol is what pairs a response with its request.
 *
 * The answer reads the ring's positions and its head header and nothing else,
 * and the trace counter is taken after the answer has been computed, so it
 * cannot affect the state the lost-wakeup argument rests on. */
static int pc_frame_ready(void *context)
{
    const PcWait *wait = context;
    int ready = 0;
    if (yan_os_transport_ring_size() != 0) {
        const uint32_t available = yan_os_transport_h2g_available();
        if (available >= wait->expected && wait->expected >= PC_HEADER_BYTES) {
            /* Header bytes 2 and 3 are the tag, little-endian. */
            const uint32_t tag = (uint32_t)pc_peek_response(2u) |
                                 ((uint32_t)pc_peek_response(3u) << 8);
            ready = tag == wait->tag;
        }
    }
    ++pc_shared.polls;
    return ready;
}

/* The ordinary driver shape: publish one request, wait for its response, take
 * it. The predicate is asked once on either exit of wait (0019), which is what
 * the reader asserts for every request it makes. */
static int pc_request(const YanOsBlockRequest *request, const uint8_t *data,
                      uint32_t expected, YanOsBlockResponse *response,
                      uint8_t *buffer, uint32_t capacity, uint32_t *length)
{
    PcWait wait = {expected, (uint32_t)request->tag};
    if (yan_os_block_submit(request, data) != 0) {
        return -1;
    }
    yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, pc_frame_ready, &wait);
    return yan_os_block_take(response, buffer, capacity, length);
}

static void pc_expect_header(const YanOsBlockResponse *response, uint8_t op,
                             uint16_t tag, uint32_t lba, uint32_t count,
                             uint32_t fail_code)
{
    if (response->op != op || response->status != YAN_OS_BLOCK_STATUS_OK ||
        response->tag != tag || response->lba != lba || response->count != count) {
        pc_fail(fail_code);
    }
}

#if PERSIST_ROLE == PC_ROLE_WRITER
/* ========================================================= the second task
 *
 * One job: make progress while the I/O task is blocked, and prove it by
 * counting. The window is opened by the I/O task's `armed` store, which sits
 * immediately before its blocking wait with no yield in between; every tick of
 * the window therefore happens while the I/O task is BLOCKED.
 *
 * At the end of the window it takes the frames that stand between the ring head
 * and the awaited reply. That is the action that frees the space the queued
 * request was waiting for, and the pump then answers it on its own - nothing
 * here rings a doorbell. Every take is checked, so a frame that arrived in the
 * wrong order is reported rather than silently accepted. */

/* One take, with the whole frame checked: header, payload length, and - when
 * `kind` is not PC_KIND_NONE - the payload bytes themselves. */
static int pc_peer_take(uint8_t op, uint32_t tag, uint32_t lba, uint32_t count,
                        uint32_t wanted_length, uint32_t kind, uint32_t *fail_code)
{
    YanOsBlockResponse response;
    uint32_t length = 0;
    if (yan_os_block_take(&response, pc_peer_buffer, PC_BLOCK_BYTES, &length) != 0) {
        *fail_code = PC_FAIL_DRAIN_FRAME;
        return -1;
    }
    if (length != wanted_length || response.op != op ||
        response.status != YAN_OS_BLOCK_STATUS_OK || (uint32_t)response.tag != tag ||
        response.lba != lba || response.count != count) {
        *fail_code = PC_FAIL_DRAIN_FRAME;
        return -1;
    }
    if (kind != PC_KIND_NONE && !pc_matches(pc_peer_buffer, pc_kind_bytes(kind))) {
        *fail_code = PC_FAIL_DRAIN_PAYLOAD;
        return -1;
    }
    return 0;
}

/* Round 0: everything the ring holds ahead of the awaited write reply. The
 * order is the order the pump served the requests in, so each frame's expected
 * tag is known: the neighbour read, the capacity queries in submission order,
 * then the filler write of the target block. */
static void pc_peer_drain_write_window(void)
{
    uint32_t fail = 0;
    uint32_t frames = 0;
    if (pc_peer_take(YAN_OS_BLOCK_OP_READ, pc_shared.drain_tag[0], pc_shared.drain_lba[0],
                     1u, PC_BLOCK_BYTES, pc_shared.drain_kind[0], &fail) == 0) {
        ++frames;
        for (uint32_t i = 0; i < PC_FILL_FRAMES; ++i) {
            if (pc_peer_take(YAN_OS_BLOCK_OP_CAPACITY, PC_TAG_CAPACITY_BASE + i, 0u, 0u,
                             8u, PC_KIND_NONE, &fail) != 0) {
                break;
            }
            ++frames;
        }
        if (fail == 0 &&
            pc_peer_take(YAN_OS_BLOCK_OP_WRITE, PC_TAG_FILL, PC_LBA_TARGET, 1u, 0u,
                         PC_KIND_NONE, &fail) == 0) {
            ++frames;
        }
    }
    pc_shared.drain_frames[0] = frames;
    pc_shared.drain_fail[0] = fail;
}

/* Round 1: the single frame at the head - the first of the two reads - after
 * which the awaited second read is the head. */
static void pc_peer_drain_read_window(void)
{
    uint32_t fail = 0;
    uint32_t frames = 0;
    if (pc_peer_take(YAN_OS_BLOCK_OP_READ, pc_shared.drain_tag[1], pc_shared.drain_lba[1],
                     1u, PC_BLOCK_BYTES, pc_shared.drain_kind[1], &fail) == 0) {
        ++frames;
    }
    pc_shared.drain_frames[1] = frames;
    pc_shared.drain_fail[1] = fail;
}

static void pc_peer_task(void *arg)
{
    (void)arg;
    for (uint32_t round = 0; round < PC_ROUNDS; ++round) {
        while (pc_shared.armed != round + 1u) {
            ++pc_shared.idle_ticks;
            yan_os_task_yield();
        }
        for (uint32_t tick = 0; tick < pc_shared.budget[round]; ++tick) {
            ++pc_shared.ticks[round];
            yan_os_task_yield();
        }
        /* No yield between the ticks and the drain: the windows below measure
         * this task's progress, and the drain belongs to the window because it
         * is what unblocks the I/O task. */
        if (round == 0) {
            pc_peer_drain_write_window();
        } else {
            pc_peer_drain_read_window();
        }
        while (pc_shared.woken != round + 1u) {
            ++pc_shared.idle_ticks;
            yan_os_task_yield();
        }
    }
    pc_shared.done = 1;
    yan_os_task_exit();
}

/* ============================================================== the writer */

static void pc_writer_task(void *arg)
{
    (void)arg;
    YanOsBlockResponse response;
    uint32_t length = 0;
    uint8_t capacity_bytes[8];

    /* ---- the device's own capacity answer -------------------------------- */
    pc_phase = 1;
    {
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_CAPACITY, 0, PC_TAG_CAPACITY, 0, 0};
        if (pc_request(&request, NULL, PC_CAPACITY_FRAME, &response, capacity_bytes,
                       (uint32_t)sizeof capacity_bytes, &length) != 0) {
            pc_fail(PC_FAIL_CAPACITY_TAKE);
        }
    }
    if (length != 8u) {
        pc_fail(PC_FAIL_CAPACITY_FRAME);
    }
    pc_expect_header(&response, YAN_OS_BLOCK_OP_CAPACITY, PC_TAG_CAPACITY, 0, 0,
                     PC_FAIL_CAPACITY_FRAME);
    if (yan_os_block_count_from_bytes(capacity_bytes) != (uint64_t)PERSIST_DISK_BLOCKS) {
        pc_fail(PC_FAIL_CAPACITY_VALUE);
    }
    pc_puts("pcombined: capacity blocks=");
    pc_put_dec((uint32_t)PERSIST_DISK_BLOCKS);
    pc_puts("\r\n");

    /* ---- the patterns are worth comparing -------------------------------- */
    pc_phase = 2;
    if (!pc_is_strong(pc_pattern_a_byte) || !pc_is_strong(pc_pattern_b_byte) ||
        !pc_is_strong(pc_pattern_c_byte) || !pc_is_strong(pc_seed_byte) ||
        !pc_differs(pc_pattern_a_byte, pc_pattern_b_byte) ||
        !pc_differs(pc_pattern_a_byte, pc_seed_byte) ||
        !pc_differs(pc_pattern_c_byte, pc_seed_byte)) {
        pc_fail(PC_FAIL_PATTERN_WEAK);
    }

    /* ---- the fast path: the reply is there before wait asks -------------- */
    pc_phase = 3;
    {
        const uint32_t peer_before =
            pc_shared.idle_ticks + pc_shared.ticks[0] + pc_shared.ticks[1];
        const uint32_t polls_before = pc_shared.polls;
        pc_fill(pc_io_buffer, pc_pattern_c_byte);
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_WRITE, 0, PC_TAG_FAST,
                                           PC_LBA_FAST, 1};
        if (yan_os_block_submit(&request, pc_io_buffer) != 0) {
            pc_fail(PC_FAIL_FAST_SUBMIT);
        }
        {
            PcWait wait = {PC_HEADER_BYTES, PC_TAG_FAST};
            yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, pc_frame_ready, &wait);
        }
        const uint32_t polls_delta = pc_shared.polls - polls_before;
        const uint32_t peer_delta =
            pc_shared.idle_ticks + pc_shared.ticks[0] + pc_shared.ticks[1] - peer_before;
        if (yan_os_block_take(&response, NULL, 0, &length) != 0) {
            pc_fail(PC_FAIL_FAST_TAKE);
        }
        pc_expect_header(&response, YAN_OS_BLOCK_OP_WRITE, PC_TAG_FAST, PC_LBA_FAST,
                         1u, PC_FAIL_FAST_FRAME);
        if (length != 0) {
            pc_fail(PC_FAIL_FAST_FRAME);
        }
        pc_puts("pcombined: fast write lba=");
        pc_put_dec(PC_LBA_FAST);
        pc_puts(" bytes=");
        pc_put_dec(PC_BLOCK_BYTES);
        pc_puts(" polls=");
        pc_put_dec(polls_delta);
        pc_puts(" peer-ticks=");
        pc_put_dec(peer_delta);
        pc_puts("\r\n");
        /* The measurement is printed before it is judged. The reply was already
         * in the ring, so wait must not have registered a waiter: no task ran
         * and the predicate was asked exactly once. */
        if (polls_delta != 1u || peer_delta != 0u) {
            pc_fail(PC_FAIL_FAST_NOT_FAST);
        }
    }
    if (yan_os_transport_h2g_available() != 0) {
        pc_fail(PC_FAIL_RING_NOT_EMPTY);
    }

    /* ---- window 0: the awaited write, blocked by flow control ------------ */
    pc_phase = 10;
    pc_shared.drain_tag[0] = PC_TAG_READ_SEED;
    pc_shared.drain_lba[0] = PC_LBA_NEIGHBOR;
    pc_shared.drain_kind[0] = PC_KIND_SEED;
    pc_shared.drain_frames[0] = 0;
    pc_shared.drain_fail[0] = (uint32_t)-1;
    {
        /* The first frame the other task drains: a read of the seeded
         * neighbour, which is also the read path's own evidence. */
        const YanOsBlockRequest read_seed = {YAN_OS_BLOCK_OP_READ, 0, PC_TAG_READ_SEED,
                                             PC_LBA_NEIGHBOR, 1};
        if (yan_os_block_submit(&read_seed, NULL) != 0) {
            pc_fail(PC_FAIL_WINDOW_SUBMIT);
        }
        /* Fill the response ring until a 16-byte reply no longer fits. The
         * capacity queries are answered in order, so the arithmetic above is a
         * property of the protocol, not of the host's timing: the pump only
         * reaches the write after it has published all of these. */
        for (uint32_t i = 0; i < PC_FILL_FRAMES; ++i) {
            const YanOsBlockRequest capacity = {
                YAN_OS_BLOCK_OP_CAPACITY, 0, (uint16_t)(PC_TAG_CAPACITY_BASE + i), 0, 0};
            if (yan_os_block_submit(&capacity, NULL) != 0) {
                pc_fail(PC_FAIL_FILL_SUBMIT);
            }
        }
        pc_fill(pc_io_buffer, pc_pattern_b_byte);
        const YanOsBlockRequest filler = {YAN_OS_BLOCK_OP_WRITE, 0, PC_TAG_FILL,
                                          PC_LBA_TARGET, 1};
        if (yan_os_block_submit(&filler, pc_io_buffer) != 0) {
            pc_fail(PC_FAIL_FILL_SUBMIT);
        }
        pc_fill(pc_io_buffer, pc_pattern_a_byte);
        const YanOsBlockRequest target = {YAN_OS_BLOCK_OP_WRITE, 0, PC_TAG_TARGET,
                                          PC_LBA_TARGET, 1};
        if (yan_os_block_submit(&target, pc_io_buffer) != 0) {
            pc_fail(PC_FAIL_WINDOW_SUBMIT);
        }
    }
    /* The pump runs at instruction boundaries, so a short spin (no yield, and
     * therefore no task switch) is enough for it to publish everything it can.
     * What is asserted is the *state*: the awaited reply must not fit. This
     * also lets the doorbell's pending interrupt be serviced before the wait
     * registers, so the wake below can only come from the reply. */
    {
        const uint32_t limit = yan_os_transport_ring_size() - 1u;
        for (volatile uint32_t spin = 0; spin < 100000u; ++spin) {
            if (yan_os_transport_h2g_available() + PC_HEADER_BYTES > limit) {
                break;
            }
        }
        const uint32_t used = yan_os_transport_h2g_available();
        pc_puts("pcombined: backpressure used=");
        pc_put_dec(used);
        pc_puts(" free=");
        pc_put_dec(limit - used);
        pc_puts(" reply=");
        pc_put_dec(PC_HEADER_BYTES);
        pc_puts("\r\n");
        if (used + PC_HEADER_BYTES <= limit) {
            pc_fail(PC_FAIL_BACKPRESSURE);
        }
    }
    pc_shared.budget[0] = PC_WINDOW_WRITE;
    pc_shared.armed = 1;
    {
        PcWait wait = {PC_HEADER_BYTES, PC_TAG_TARGET};
        const uint32_t polls_before = pc_shared.polls;
        yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, pc_frame_ready, &wait);
        const uint32_t polls_delta = pc_shared.polls - polls_before;
        const uint32_t ticks = pc_shared.ticks[0];
        const uint32_t frames = pc_shared.drain_frames[0];
        pc_puts("pcombined: window round=0 kind=write budget=");
        pc_put_dec(PC_WINDOW_WRITE);
        pc_puts(" ticks=");
        pc_put_dec(ticks);
        pc_puts(" polls=");
        pc_put_dec(polls_delta);
        pc_puts(" frames=");
        pc_put_dec(frames);
        pc_puts("\r\n");
        /* The window has to have elapsed, or wait returned before the event. */
        if (ticks != PC_WINDOW_WRITE) {
            pc_fail(PC_FAIL_NO_WINDOW);
        }
        /* The discriminating assertion: a blocked task is asked exactly once. */
        if (polls_delta != 1u) {
            pc_fail(PC_FAIL_POLLED_WHILE_BLOCKED);
        }
        /* The wake required the other task to have freed the ring first. */
        if (pc_shared.drain_fail[0] == (uint32_t)-1) {
            pc_fail(PC_FAIL_DRAIN_MISSING);
        }
        if (pc_shared.drain_fail[0] != 0u) {
            pc_fail(pc_shared.drain_fail[0]);
        }
        if (frames != PC_FRAMES_BEFORE_TARGET) {
            pc_fail(PC_FAIL_FRAME_COUNT);
        }
        /* Only os/trap_entry.S's handler turns a BLOCKED task RUNNABLE, and
         * 0019's ISR services and acks the device before it does: the level is
         * withdrawn by the time this task runs again. */
        if ((yan_os_transport_irq_status() & YAN_OS_TRANSPORT_IRQ_H2G_DATA) != 0) {
            pc_fail(PC_FAIL_ISR_NOT_SERVICED);
        }
    }
    /* The replied frame is the only one left, and it is the one the interrupt
     * announced: taking it must deliver the awaited write. */
    if (yan_os_block_take(&response, NULL, 0, &length) != 0) {
        pc_fail(PC_FAIL_WINDOW_TAKE);
    }
    pc_expect_header(&response, YAN_OS_BLOCK_OP_WRITE, PC_TAG_TARGET, PC_LBA_TARGET,
                     1u, PC_FAIL_WINDOW_FRAME);
    if (length != 0) {
        pc_fail(PC_FAIL_WINDOW_FRAME);
    }
    pc_puts("pcombined: window round=0 awaited=write lba=");
    pc_put_dec(PC_LBA_TARGET);
    pc_puts(" status=ok count=1\r\n");
    /* Release the other task into round 1 only now: its round 0 is over when
     * the frame it drained has been accounted for here. */
    pc_shared.woken = 1u;
    if (yan_os_transport_h2g_available() != 0) {
        pc_fail(PC_FAIL_RING_NOT_EMPTY);
    }

    /* ---- window 1: the read-back, waited for the same way ---------------- */
    pc_phase = 20;
    pc_shared.drain_tag[1] = PC_TAG_READ_FIRST;
    pc_shared.drain_lba[1] = PC_LBA_TARGET;
    pc_shared.drain_kind[1] = PC_KIND_PATTERN_A;
    pc_shared.drain_frames[1] = 0;
    pc_shared.drain_fail[1] = (uint32_t)-1;
    {
        const YanOsBlockRequest first = {YAN_OS_BLOCK_OP_READ, 0, PC_TAG_READ_FIRST,
                                         PC_LBA_TARGET, 1};
        const YanOsBlockRequest second = {YAN_OS_BLOCK_OP_READ, 0, PC_TAG_READ_SECOND,
                                          PC_LBA_NEIGHBOR, 1};
        if (yan_os_block_submit(&first, NULL) != 0 ||
            yan_os_block_submit(&second, NULL) != 0) {
            pc_fail(PC_FAIL_WINDOW_SUBMIT);
        }
    }
    pc_shared.budget[1] = PC_WINDOW_READ;
    pc_shared.armed = 2;
    {
        PcWait wait = {PC_READ_FRAME, PC_TAG_READ_SECOND};
        const uint32_t polls_before = pc_shared.polls;
        yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, pc_frame_ready, &wait);
        const uint32_t polls_delta = pc_shared.polls - polls_before;
        const uint32_t ticks = pc_shared.ticks[1];
        pc_puts("pcombined: window round=1 kind=read budget=");
        pc_put_dec(PC_WINDOW_READ);
        pc_puts(" ticks=");
        pc_put_dec(ticks);
        pc_puts(" polls=");
        pc_put_dec(polls_delta);
        pc_puts(" frames=");
        pc_put_dec(pc_shared.drain_frames[1]);
        pc_puts("\r\n");
        if (ticks != PC_WINDOW_READ) {
            pc_fail(PC_FAIL_NO_WINDOW);
        }
        if (polls_delta != 1u) {
            pc_fail(PC_FAIL_POLLED_WHILE_BLOCKED);
        }
        if (pc_shared.drain_fail[1] == (uint32_t)-1) {
            pc_fail(PC_FAIL_DRAIN_MISSING);
        }
        if (pc_shared.drain_fail[1] != 0u) {
            pc_fail(pc_shared.drain_fail[1]);
        }
        if (pc_shared.drain_frames[1] != 1u) {
            pc_fail(PC_FAIL_FRAME_COUNT);
        }
        if ((yan_os_transport_irq_status() & YAN_OS_TRANSPORT_IRQ_H2G_DATA) != 0) {
            pc_fail(PC_FAIL_ISR_NOT_SERVICED);
        }
    }
    pc_phase = 21;
    if (yan_os_block_take(&response, pc_io_buffer, PC_BLOCK_BYTES, &length) != 0 ||
        length != PC_BLOCK_BYTES) {
        pc_fail(PC_FAIL_WINDOW_TAKE);
    }
    pc_expect_header(&response, YAN_OS_BLOCK_OP_READ, PC_TAG_READ_SECOND,
                     PC_LBA_NEIGHBOR, 1u, PC_FAIL_WINDOW_FRAME);
    if (!pc_matches(pc_io_buffer, pc_seed_byte)) {
        pc_fail(PC_FAIL_WINDOW_FRAME);
    }
    pc_puts("pcombined: window round=1 read-back lba=");
    pc_put_dec(PC_LBA_TARGET);
    pc_puts(" peer-read=pattern-a match=1\r\n");
    pc_shared.woken = 2;
    if (yan_os_transport_h2g_available() != 0) {
        pc_fail(PC_FAIL_RING_NOT_EMPTY);
    }

    /* ---- the other task's own verdict, then the end ---------------------- */
    pc_phase = 30;
    {
        uint32_t spins = 0;
        while (pc_shared.done == 0 && spins < 100000u) {
            ++spins;
            yan_os_task_yield();
        }
        if (pc_shared.done == 0) {
            pc_fail(PC_FAIL_PROGRESS);
        }
    }
    pc_puts("pcombined: PASS\r\n");
    guest_finish(1u);
    for (;;) {
    }
}

#else /* ------------------------------------------------------------- reader */

static void pc_reader_task(void *arg)
{
    (void)arg;
    YanOsBlockResponse response;
    uint32_t length = 0;
    uint8_t capacity_bytes[8];

    pc_phase = 1;
    {
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_CAPACITY, 0, PC_TAG_CAPACITY, 0, 0};
        const uint32_t polls_before = pc_shared.polls;
        if (pc_request(&request, NULL, PC_CAPACITY_FRAME, &response, capacity_bytes,
                       (uint32_t)sizeof capacity_bytes, &length) != 0) {
            pc_fail(PC_FAIL_CAPACITY_TAKE);
        }
        if (pc_shared.polls - polls_before != 1u) {
            pc_fail(PC_FAIL_FAST_NOT_FAST);
        }
    }
    if (length != 8u) {
        pc_fail(PC_FAIL_CAPACITY_FRAME);
    }
    pc_expect_header(&response, YAN_OS_BLOCK_OP_CAPACITY, PC_TAG_CAPACITY, 0, 0,
                     PC_FAIL_CAPACITY_FRAME);
    if (yan_os_block_count_from_bytes(capacity_bytes) != (uint64_t)PERSIST_DISK_BLOCKS) {
        pc_fail(PC_FAIL_CAPACITY_VALUE);
    }
    pc_puts("pcombined: reader capacity blocks=");
    pc_put_dec((uint32_t)PERSIST_DISK_BLOCKS);
    pc_puts("\r\n");

    /* Every block the writer claimed is read back byte for byte, in this fresh
     * process, through the same protocol and the same file backend. The
     * neighbour block is compared against the harness's seed: a write that
     * landed next door cannot pass both. */
    static const struct {
        uint32_t lba;
        uint16_t tag;
        PcByteFn byte;
        const char *label;
    } wanted[] = {
        {PC_LBA_TARGET, 0x0401u, pc_pattern_a_byte, "pattern-a"},
        {PC_LBA_FAST, 0x0402u, pc_pattern_c_byte, "pattern-c"},
        {PC_LBA_NEIGHBOR, 0x0403u, pc_seed_byte, "seed"},
    };
    for (uint32_t index = 0; index < sizeof wanted / sizeof wanted[0]; ++index) {
        pc_phase = 10u + index;
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_READ, 0, wanted[index].tag,
                                           wanted[index].lba, 1};
        const uint32_t polls_before = pc_shared.polls;
        if (pc_request(&request, NULL, PC_READ_FRAME, &response, pc_io_buffer,
                       PC_BLOCK_BYTES, &length) != 0 ||
            length != PC_BLOCK_BYTES) {
            pc_fail(PC_FAIL_READ_TAKE);
        }
        if (pc_shared.polls - polls_before != 1u) {
            pc_fail(PC_FAIL_FAST_NOT_FAST);
        }
        pc_expect_header(&response, YAN_OS_BLOCK_OP_READ, wanted[index].tag,
                         wanted[index].lba, 1u, PC_FAIL_READ_FRAME);
        if (!pc_matches(pc_io_buffer, wanted[index].byte)) {
            pc_fail(PC_FAIL_READ_PAYLOAD);
        }
        pc_puts("pcombined: reader lba=");
        pc_put_dec(wanted[index].lba);
        pc_puts(" bytes=");
        pc_put_dec(PC_BLOCK_BYTES);
        pc_puts(" match=1 pattern=");
        pc_puts(wanted[index].label);
        pc_puts("\r\n");
    }
    pc_puts("pcombined: reader PASS\r\n");
    guest_finish(1u);
    for (;;) {
    }
}

#endif

/* ==================================================================== main */

/* The IRQ route below the CPU is the caller's business (0019 SPEC): the device's
 * IRQ_ENABLE and the PLIC's enable / priority / threshold. mie.MEIE belongs to
 * the runtime and is not touched here. */
static void pc_configure_route(void)
{
    yan_os_transport_set_irq_enable(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_set_threshold(0);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
}

int main(void)
{
#if PERSIST_ROLE == PC_ROLE_WRITER
    pc_puts("pcombined: writer: cooperative block I/O on a disk image\r\n");
#else
    pc_puts("pcombined: reader: a fresh process reads the same image\r\n");
#endif
    /* Without --disk-image the channel has no ring and no notify callback, so
     * HOST_READY is 0 and the guest must not touch the rings (0014). Failing
     * here names the missing host pump instead of timing out. */
    if (!yan_os_transport_host_ready()) {
        pc_fail(PC_FAIL_NO_CHANNEL);
    }
    /* 0018's ring floor: RING_SIZE 8192 carries one block, and both windows are
     * written against that geometry (two 4112-byte read responses do not fit in
     * the 8191-byte ring, which is the flow control they are built on). */
    if (yan_os_block_max_count() != 1u) {
        pc_fail(PC_FAIL_MAX_COUNT);
    }
    pc_configure_route();
#if PERSIST_ROLE == PC_ROLE_WRITER
    if (yan_os_task_spawn(pc_writer_task, NULL) != YAN_OS_TASK_OK ||
        yan_os_task_spawn(pc_peer_task, NULL) != YAN_OS_TASK_OK) {
        pc_fail(PC_FAIL_SPAWN);
    }
#else
    if (yan_os_task_spawn(pc_reader_task, NULL) != YAN_OS_TASK_OK) {
        pc_fail(PC_FAIL_SPAWN);
    }
#endif
    yan_os_sched_run();
    pc_fail(PC_FAIL_PROGRESS); /* the scheduler never returns */
}
