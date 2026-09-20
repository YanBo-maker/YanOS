/* Combination acceptance case for M2a: block I/O requested by a Guest *task*
 * and waited for by the cooperative runtime.
 *
 * What this image is for
 * ----------------------
 * docs/specs/0018-block-protocol.md (os/block.c, tools/host_block.c) and
 * docs/specs/0019-cooperative-runtime.md (os/task.c) were each verified on
 * their own, but never together: the block suite links tests/guest/mtrap_entry.S
 * and has no runtime, and the runtime suite never speaks the block protocol.
 * 0019's own INTENTION names the combination as the reason the runtime exists -
 * a block request is asynchronous, so without a way to give the CPU up a driver
 * can only spin in place. This image is that combination, and it is built to
 * fail if the CPU is not actually handed over (see "the discriminating
 * assertion" below).
 *
 * The image links the *runtime* trap entry (os/trap_entry.S), never
 * tests/guest/mtrap_entry.S: both define _start, the vector, tohost and a boot
 * stack, so linking both is a duplicate-symbol error. It also means the
 * yan_guest_* helpers of tests/guest/mtrap.c are not available here - they
 * belong to the other frame layout.
 *
 * What is asserted
 * ----------------
 *   1. correctness   capacity query, two writes and their read-back, all
 *                    through os/block.c, with the payload compared byte for
 *                    byte against what was written. The disk is zeroed by
 *                    yan_run (calloc), so a zero-filled block could never pass
 *                    the payload comparison: the patterns carry >= 200 distinct
 *                    byte values, block 0 is never written, and the case reads
 *                    it back as all zeroes.
 *   2. the wait      every request is answered through
 *                    yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, predicate, ...).
 *   3. the block     the discriminating assertion (below).
 *
 * The blocking window, and why it is flow control and nothing else
 * ----------------------------------------------------------------
 * At this ring geometry (RING_SIZE 8192, so yan_os_block_max_count() == 1) a
 * one-block read response is 4112 bytes and the response ring holds 8191 of
 * them, so two cannot be in flight at once. That is the back-pressure the
 * window is built on:
 *
 *   1. the I/O task submits R1 (read) and then R2 (read). The pump answers R1,
 *      whose 4112-byte response leaves 4079 free bytes; R2's response does not
 *      fit, so host_block.c's flow-control check declines it and the request
 *      stays in the request ring;
 *   2. the I/O task hands the CPU over with
 *      yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, predicate, ...). The predicate
 *      asks for *its own* response - the frame whose tag is R2's - and the ring
 *      holds R1's, so it answers no and the task is marked BLOCKED;
 *   3. the second task, already past its handshake, spends the whole window
 *      yielding (every tick of the window therefore happens while the I/O task
 *      is BLOCKED) and then takes R1's response. That is what frees the ring;
 *   4. the pump answers R2 by itself at the next instruction boundary - no
 *      doorbell is rung by anybody after step 1. Publishing the reply asserts
 *      the channel line, the PLIC turns it into MEIP, and os/trap_entry.S's
 *      handler acks the device and wakes the waiter;
 *   5. the I/O task takes R2's response and compares its payload byte for byte
 *      against the pattern that was written to that block.
 *
 * Two earlier versions of this case leaned on host behaviour instead of on flow
 * control. Both were removed rather than kept:
 *
 *   * "the pump only retries when the Guest rings again" - that was a real
 *     defect in tools/yan_run.c (an accepted request could be stranded forever
 *     while the Guest slept in wait), and it has been fixed: the pump now also
 *     serves while the request ring still holds an unconsumed frame. This image
 *     is the regression test for that fix. It rings no doorbell of its own
 *     after the two submits, so a pump that went back to being edge triggered
 *     would leave the I/O task blocked forever and the run would reach no
 *     verdict at all.
 *   * "the waiter takes the first response itself and then finds the ring
 *     empty" - that only blocked while the pump failed to retry, so it stopped
 *     blocking the moment the pump was fixed. It is gone.
 *
 * Detection power for the F1 fix in tools/yan_run.c
 * ------------------------------------------------
 * [Out-of-suite record: run when the case was written, 2026-09-21. It is written
 * down here because the claim "this case covers the pump's retry" is a measured
 * claim, not an assumption, and it should stay checkable.]
 *
 * Revert the fix in a *private copy* of the tool and rerun the case against it:
 *
 *     cp tools/yan_run.c /tmp/yan_run_edge.c
 *     # in the copy, replace the pump's condition
 *     #   if (disk_attached &&
 *     #       (doorbell_rung ||
 *     #        yan_transport_host_readable(&machine.transport) > 0)) {
 *     # with the edge-triggered form it had before the fix
 *     #   if (disk_attached && doorbell_rung) {
 *     cc -O1 -std=c17 -I include -I tools -o /tmp/yan_run_edge \
 *         /tmp/yan_run_edge.c tools/host_file.c tools/host_terminal.c \
 *         tools/host_block.c "src/"*.c
 *     bash tests/guest/run_m2a_combined.sh --source . \
 *         --gcc /usr/bin/riscv64-linux-gnu-gcc --run /tmp/yan_run_edge \
 *         --work /tmp/m2a-edge --max-steps 3000000
 *
 * and the case ends without a verdict:
 *
 *     INCONCLUSIVE combined case: no verdict within 3000000 instructions
 *         m2a: read lba=0 zero=1
 *         m2a: sequential-waits idle_ticks=0
 *     yan_run: stopped after 3000000 steps without reaching tohost
 *
 * That is the point of the experiment: with the retry gone, the reply to the
 * queued read is never published, so the run stops exactly where the window
 * opens. Put the other way round, if someone changes tools/yan_run.c back to
 * serving only on the doorbell edge, this case fails - which is what makes it
 * the regression test for that fix, and why it rings no doorbell of its own.
 *
 * The discriminating assertion
 * ----------------------------
 * "The read succeeded" is not evidence of anything: a runtime that fakes wait
 * as "yield and poll again" passes every functional check in this image,
 * because the response does arrive either way. What separates the two is
 * whether the waiting task is *scheduled* while it waits.
 *
 * The measured quantities are:
 *   * ticks   - how many times the other task completed a round inside the
 *               window; every one of them happened while this task was BLOCKED
 *               (the handshake store is immediately before the blocking wait,
 *               with no yield between them);
 *   * polls   - how many times the runtime asked this task's predicate during
 *               that wait. A block-and-wake runtime asks exactly once (SPEC
 *               step 2) and never again: the predicate is not re-checked on the
 *               way out. A "yield + busy-wait" runtime asks once per round of
 *               its loop, so polls tracks ticks;
 *   * coupling- two rounds with different window sizes (32 and 96) show that
 *               polls does not scale with the window: 1 and 1, not 32 and 96.
 *
 * The same assertion is what kills the planted defect in
 * tests/guest/run_m2a_combined.sh --mutation: a private copy of os/task.c whose
 * yan_os_task_wait() is "while (!predicate(context)) yan_os_task_yield();"
 * still passes every functional check here and fails on
 * M2A_FAIL_POLLED_WHILE_BLOCKED.
 *
 * Why the predicate must not call take()
 * --------------------------------------
 * yan_os_block_take() consumes the frame on its success path; only AGAIN is
 * free of side effects. A predicate that returns "the response is here" by
 * calling take() has therefore already eaten the response by the time wait()
 * inspects the answer, and the caller's own take() finds AGAIN: the response is
 * lost silently, with nothing left in the ring to recover it from. The last
 * phase of this image is that counterexample, run on the real path, and it
 * asserts the loss (the predicate consumed a valid frame; the caller's take
 * then answers AGAIN). It is a documentation case, not a supported pattern.
 *
 * The predicate's own contract (os/task.h): short, read-only, no side effects.
 * The decision here is a pure read: the response ring's positions and the
 * 16-byte header of the frame at its head, with the same peek os/block.c uses
 * on the framing side. That is required, not stylistic - 0019's lost-wakeup
 * argument depends on the predicate answering without changing any state the
 * event or the ring depends on, and it answers inside the critical section.
 *
 * The one extra store is the trace counter the assertion above is built on. It
 * is test-local memory that the runtime never reads, it is taken *after* the
 * answer has been computed, and it cannot affect the device or ring state the
 * predicate observes - so it does not touch the lost-wakeup argument, which
 * rests on the query being read-only and on "register + block" being atomic,
 * and on nothing else. It is also the only place test-owned code can run inside
 * a wait at all: a task that is really blocked executes nothing of its own.
 * This is the same probe technique 0019's own runtime check uses
 * (rt_probe.predicate_calls in tests/guest/runtime_check.c).
 *
 * Verdicts (0013): tohost 1 means the case passed; anything else is a failure
 * code whose top bit is set. The case's own codes carry 0x40000000 so a host
 * can tell them from the runtime's panic codes (0x80000000 | reason).
 *
 * Build and run: tests/guest/run_m2a_combined.sh. The production path is
 * `yan_run --disk BLOCKS --terminal`, which is where the ring geometry, the
 * notify callback and publish -> PLIC -> MEIP all come from. */

#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "console.h"
#include "guest.h"
#include "platform.h"
#include "task.h"

/* ------------------------------------------------------------- constants */

/* The block count the harness attaches with `yan_run --disk N`. The capacity
 * query is checked against it, so the case proves the query read the *device's*
 * answer rather than a constant. */
#ifndef YAN_M2A_DISK_BLOCKS
#define YAN_M2A_DISK_BLOCKS 8u
#endif

#define M2A_BLOCK_BYTES ((uint32_t)YAN_OS_BLOCK_BLOCK_SIZE)
#define M2A_HEADER_BYTES ((uint32_t)YAN_OS_BLOCK_HEADER_SIZE)
/* A one-block read response, which is also the largest frame the ring carries
 * at 0018's floor (RING_SIZE 8192 -> max_count 1). Two of these do not fit in
 * the ring, which is the flow control the window is built on. */
#define M2A_READ_FRAME (M2A_HEADER_BYTES + M2A_BLOCK_BYTES)
#define M2A_CAPACITY_FRAME (M2A_HEADER_BYTES + 8u)

/* The two windows. Different sizes on purpose: the point of the second round is
 * that the waiting task's poll count does not grow with it. */
#define M2A_ROUNDS 2u
#define M2A_WINDOW_FIRST 32u
#define M2A_WINDOW_SECOND 96u

/* Block numbers: 0 is deliberately never written, so it can be read back as an
 * untouched block. */
#define M2A_LBA_PATTERN_A 1u
#define M2A_LBA_PATTERN_B 2u
#define M2A_LBA_UNWRITTEN 0u

/* ------------------------------------------------------------ fail codes */

#define M2A_FAIL(reason) (UINT32_C(0x40000000) | (reason))

#define M2A_FAIL_NO_CHANNEL 1u          /* no host pump: run it with --disk    */
#define M2A_FAIL_MAX_COUNT 2u           /* the ring geometry is not 8192       */
#define M2A_FAIL_SPAWN 3u               /* the runtime refused a task slot     */
#define M2A_FAIL_CAPACITY_TAKE 4u       /* the capacity response was not taken */
#define M2A_FAIL_CAPACITY_FRAME 5u      /* its header did not match            */
#define M2A_FAIL_CAPACITY_VALUE 6u      /* the device reported another size    */
#define M2A_FAIL_WRITE_SUBMIT 7u        /* a write request was not published   */
#define M2A_FAIL_WRITE_FRAME 8u         /* its response did not match          */
#define M2A_FAIL_READ_SUBMIT 9u         /* a read request was not published    */
#define M2A_FAIL_READ_TAKE 10u          /* its response was not delivered      */
#define M2A_FAIL_PAYLOAD 11u            /* the payload is not what was written */
#define M2A_FAIL_UNWRITTEN_BLOCK 12u    /* block 0 came back as something else */
#define M2A_FAIL_PATTERN_WEAK 13u       /* the test's own pattern proves nothing */
#define M2A_FAIL_POLLED_WHILE_BLOCKED 14u /* the waiter ran while it was blocked */
#define M2A_FAIL_NO_WINDOW 15u          /* wait returned before the window ran */
#define M2A_FAIL_ROUND_SUBMIT 16u       /* a queued request was not published  */
#define M2A_FAIL_ROUND_TAKE 17u         /* the queued response was not there   */
#define M2A_FAIL_ROUND_FRAME 18u        /* tag / lba / length did not match    */
#define M2A_FAIL_RING_NOT_EMPTY 19u     /* a frame of the previous round was left */
#define M2A_FAIL_PROGRESS 20u           /* the other task never finished       */
#define M2A_FAIL_TAKE_PREDICATE 21u     /* the documented trap: frame consumed */
#define M2A_FAIL_TRAP_SETUP 22u         /* the trap phase's frame never arrived */
#define M2A_FAIL_ISR_NOT_SERVICED 23u   /* the wake left the device asserted    */
#define M2A_FAIL_DRAIN_TAKE 24u         /* the peer could not take the first frame */
#define M2A_FAIL_DRAIN_FRAME 25u        /* the peer took the wrong first frame */
#define M2A_FAIL_DRAIN_PAYLOAD 26u      /* the peer's frame had the wrong bytes */
#define M2A_FAIL_DRAIN_MISSING 27u      /* the wake happened without the drain */
#define M2A_FAIL_TRAP_PREDICATE 28u     /* the trap predicate did not consume  */

/* ------------------------------------------------------------ the console */

/* The console of 0017 never blocks and reports YAN_OS_UNAVAILABLE when no
 * terminal is attached, so a headless run keeps going with no diagnostics
 * instead of hanging. `yan_run --terminal` is what makes these lines visible,
 * and they are the evidence the harness greps for. */
static void m2a_puts(const char *text)
{
    (void)yan_os_console_puts(text);
}

static void m2a_put_hex(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    (void)yan_os_console_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        (void)yan_os_console_putc(digits[(value >> (unsigned)shift) & 0xfu]);
    }
}

static void m2a_put_dec(uint32_t value)
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

__attribute__((noreturn)) static void m2a_fail(uint32_t reason)
{
    m2a_puts("m2a: FAIL code=");
    m2a_put_hex(M2A_FAIL(reason));
    m2a_puts("\r\n");
    guest_finish(M2A_FAIL(reason));
    for (;;) {
    }
}

/* ---------------------------------------------------------------- shared */

/* Between the two tasks. volatile because one task reads what the other wrote;
 * there is a single hart, so the only thing that matters is that the compiler
 * does not cache a value across a yield. */
typedef struct {
    uint32_t polls;                /* every predicate call, after the answer   */
    uint32_t armed;                /* round the I/O task has armed, 0 = none   */
    uint32_t window[M2A_ROUNDS];   /* the tick budget the I/O task asked for   */
    uint32_t window_ticks[M2A_ROUNDS]; /* rounds the other task completed in it */
    uint32_t woken;                /* round whose queued frame was taken       */
    uint32_t idle_ticks;           /* the other task's ticks outside a window  */
    uint32_t done;                 /* the other task finished both windows     */
    uint32_t phase;                /* progress marker for a failing run        */
    /* The other task's half of the driver: which first frame to take for the
     * round, and the failure code it found (0 = took it and it was correct). */
    uint32_t drain_tag[M2A_ROUNDS];
    uint32_t drain_lba[M2A_ROUNDS];
    uint32_t drain_fail[M2A_ROUNDS];
} M2aShared;

static volatile M2aShared m2a_shared;

static uint8_t m2a_pattern_a[M2A_BLOCK_BYTES];
static uint8_t m2a_pattern_b[M2A_BLOCK_BYTES];
static uint8_t m2a_read_buffer[M2A_BLOCK_BYTES];
/* The other task has its own buffer: the two hold a 4 KiB block at different
 * moments, and sharing one would make the case depend on the order being the
 * one it expects instead of asserting it. */
static uint8_t m2a_peer_buffer[M2A_BLOCK_BYTES];

/* ------------------------------------------------------------- predicate */

typedef struct {
    uint32_t expected; /* bytes a complete response frame occupies */
    uint32_t tag;      /* the tag of the response being waited for */
} M2aWait;

/* The i-th byte of the frame at the head of the response ring, read without
 * consuming anything. The same shape as os/block.c's peek_response: the ring is
 * ordinary RAM at ring_base + ring_size, and the index is taken modulo the ring
 * size, so a header that crosses the wrap point reads back contiguous. No ring
 * position is written here. */
static uint8_t m2a_peek_response(uint32_t index)
{
    const uint32_t size = yan_os_transport_ring_size();
    const volatile uint8_t *ring =
        (const volatile uint8_t *)(uintptr_t)(yan_os_transport_ring_base() + size);
    return ring[yan_os_ring_index(yan_os_transport_h2g_tail(), size, index)];
}

/* The wait predicate.
 *
 * Two conditions, because "a complete response has arrived" is not the same as
 * "the response this task is waiting for has arrived". While the I/O task waits
 * in a round, the ring already holds the response to the *first* read of the
 * pair - nobody has taken it yet, and taking it is the other task's job. The
 * frame whose tag matches is the answer, and the tag is what 0018 keeps in the
 * protocol for exactly this pairing: the response echoes the request's tag.
 *
 * The answer reads the ring's positions and its head header and nothing else,
 * and the trace counter is taken after the answer is computed (see the file
 * header for why that keeps the contract). */
static int m2a_frame_ready(void *context)
{
    const M2aWait *wait = context;
    int ready = 0;
    if (yan_os_transport_ring_size() != 0) {
        const uint32_t available = yan_os_transport_h2g_available();
        if (available >= wait->expected && wait->expected >= M2A_HEADER_BYTES) {
            /* Header bytes 2 and 3 are the tag, little-endian. */
            const uint32_t tag = (uint32_t)m2a_peek_response(2u) |
                                 ((uint32_t)m2a_peek_response(3u) << 8);
            ready = tag == wait->tag;
        }
    }
    ++m2a_shared.polls;
    return ready;
}

/* ------------------------------------------------------------- patterns */

/* A 32-bit LCG: no 64-bit arithmetic at all, because a freestanding rv32 image
 * is linked without libgcc (a 64-bit multiply or a variable shift would become
 * a call to __muldi3 / __ashldi3 that nothing provides). */
static void m2a_fill_pattern(uint8_t *buffer, uint32_t seed)
{
    uint32_t state = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    for (uint32_t i = 0; i < M2A_BLOCK_BYTES; ++i) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        buffer[i] = (uint8_t)(state >> 24);
    }
}

/* The payload comparison is only evidence if the payload could not have been
 * produced by accident: this asserts the pattern is not a constant block, so a
 * zero-filled response (or a single repeated byte) can never match it. */
static int m2a_pattern_is_strong(const uint8_t *buffer)
{
    uint8_t seen[256];
    uint32_t distinct = 0;
    memset(seen, 0, sizeof seen);
    for (uint32_t i = 0; i < M2A_BLOCK_BYTES; ++i) {
        if (seen[buffer[i]] == 0) {
            seen[buffer[i]] = 1;
            ++distinct;
        }
    }
    return distinct >= 200u;
}

static int m2a_all_zero(const uint8_t *buffer)
{
    for (uint32_t i = 0; i < M2A_BLOCK_BYTES; ++i) {
        if (buffer[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static const uint8_t *m2a_pattern_for(uint32_t lba)
{
    return lba == M2A_LBA_PATTERN_A ? m2a_pattern_a : m2a_pattern_b;
}

/* ------------------------------------------------------------- one I/O */

/* The ordinary driver shape: publish one request, wait for its response, take
 * it. `expected` is the whole response frame and the tag names it, which is
 * what makes the predicate mean "the frame this request asked for is here" -
 * the framing rule the layer above (os/block.c) is built on. */
static int m2a_request(const YanOsBlockRequest *request, const uint8_t *data,
                       uint32_t expected, YanOsBlockResponse *response,
                       uint8_t *buffer, uint32_t capacity, uint32_t *length)
{
    M2aWait wait = {expected, (uint32_t)request->tag};
    if (yan_os_block_submit(request, data) != 0) {
        return -1;
    }
    yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, m2a_frame_ready, &wait);
    return yan_os_block_take(response, buffer, capacity, length);
}

static void m2a_expect_header(const YanOsBlockResponse *response, uint8_t op,
                              uint16_t tag, uint32_t lba, uint32_t count,
                              uint32_t fail_code)
{
    if (response->op != op || response->status != YAN_OS_BLOCK_STATUS_OK ||
        response->tag != tag || response->lba != lba || response->count != count) {
        m2a_fail(fail_code);
    }
}

/* ========================================================= the second task
 *
 * One job: make progress while the I/O task is blocked, and prove it by
 * counting. The window is opened by the I/O task's `armed` store, which sits
 * immediately before its blocking wait with no yield in between; every tick of
 * the window therefore happens while the I/O task is BLOCKED.
 *
 * At the end of the window it takes the round's first response. That is the
 * action that frees the response ring, and the pump then answers the queued read
 * on its own - by retrying while the request ring still holds an unconsumed
 * frame. Nothing here rings a doorbell: if the run needed one, the window would
 * depend on the pump's trigger instead of on flow control. */
static int m2a_take_first(uint32_t tag, uint32_t lba, uint32_t *fail_code)
{
    YanOsBlockResponse response;
    uint32_t length = 0;
    if (yan_os_block_take(&response, m2a_peer_buffer, M2A_BLOCK_BYTES, &length) != 0) {
        *fail_code = M2A_FAIL_DRAIN_TAKE;
        return -1;
    }
    if (length != M2A_BLOCK_BYTES || response.op != YAN_OS_BLOCK_OP_READ ||
        response.status != YAN_OS_BLOCK_STATUS_OK || (uint32_t)response.tag != tag ||
        response.lba != lba || response.count != 1u) {
        *fail_code = M2A_FAIL_DRAIN_FRAME;
        return -1;
    }
    if (memcmp(m2a_peer_buffer, m2a_pattern_for(lba), M2A_BLOCK_BYTES) != 0) {
        *fail_code = M2A_FAIL_DRAIN_PAYLOAD;
        return -1;
    }
    return 0;
}

static void m2a_progress_task(void *arg)
{
    (void)arg;
    for (uint32_t round = 0; round < M2A_ROUNDS; ++round) {
        while (m2a_shared.armed != round + 1u) {
            ++m2a_shared.idle_ticks;
            yan_os_task_yield();
        }
        for (uint32_t tick = 0; tick < m2a_shared.window[round]; ++tick) {
            ++m2a_shared.window_ticks[round];
            yan_os_task_yield();
        }
        uint32_t drain_fail = 0;
        (void)m2a_take_first(m2a_shared.drain_tag[round], m2a_shared.drain_lba[round],
                             &drain_fail);
        m2a_shared.drain_fail[round] = drain_fail;
        while (m2a_shared.woken != round + 1u) {
            ++m2a_shared.idle_ticks;
            yan_os_task_yield();
        }
    }
    m2a_shared.done = 1;
    yan_os_task_exit();
}

/* ============================================================ the trap case
 *
 * yan_os_block_take() as the predicate: the dangerous pattern, run on purpose.
 * take() consumes the frame when it succeeds, so the predicate that answers
 * "yes" has already taken it - and wait() only checks the answer, it does not
 * re-run the caller's take. The response is gone, with nothing left to recover
 * it from. This is a documentation case; the phase asserts the loss instead of
 * pretending the pattern works. */
typedef struct {
    YanOsBlockResponse response;
    uint32_t length;
    uint32_t calls;
    int result;
} M2aTakePredicate;

static int m2a_frame_ready_by_taking(void *context)
{
    M2aTakePredicate *state = context;
    ++state->calls;
    state->result = yan_os_block_take(&state->response, m2a_read_buffer,
                                      M2A_BLOCK_BYTES, &state->length);
    return state->result == 0;
}

/* ============================================================== the I/O task */

static void m2a_io_task(void *arg)
{
    (void)arg;
    YanOsBlockResponse response;
    uint32_t length = 0;
    uint8_t capacity_bytes[8];

    /* ---- capacity query ------------------------------------------------- */
    m2a_shared.phase = 1;
    {
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_CAPACITY, 0, 0x0101u, 0, 0};
        if (m2a_request(&request, NULL, M2A_CAPACITY_FRAME, &response, capacity_bytes,
                        (uint32_t)sizeof capacity_bytes, &length) != 0) {
            m2a_fail(M2A_FAIL_CAPACITY_TAKE);
        }
    }
    if (length != 8u) {
        m2a_fail(M2A_FAIL_CAPACITY_FRAME);
    }
    m2a_expect_header(&response, YAN_OS_BLOCK_OP_CAPACITY, 0x0101u, 0, 0,
                      M2A_FAIL_CAPACITY_FRAME);
    /* The payload is a little-endian u64; os/block.h decodes it without libgcc. */
    if (yan_os_block_count_from_bytes(capacity_bytes) != (uint64_t)YAN_M2A_DISK_BLOCKS) {
        m2a_fail(M2A_FAIL_CAPACITY_VALUE);
    }
    m2a_puts("m2a: capacity blocks=");
    m2a_put_dec((uint32_t)YAN_M2A_DISK_BLOCKS);
    m2a_puts("\r\n");

    /* ---- write, then read it back --------------------------------------- */
    m2a_shared.phase = 2;
    m2a_fill_pattern(m2a_pattern_a, UINT32_C(0x1111));
    m2a_fill_pattern(m2a_pattern_b, UINT32_C(0x2222));
    if (!m2a_pattern_is_strong(m2a_pattern_a) || !m2a_pattern_is_strong(m2a_pattern_b) ||
        memcmp(m2a_pattern_a, m2a_pattern_b, M2A_BLOCK_BYTES) == 0) {
        m2a_fail(M2A_FAIL_PATTERN_WEAK);
    }
    for (uint32_t index = 0; index < 2u; ++index) {
        const uint32_t lba = index == 0 ? M2A_LBA_PATTERN_A : M2A_LBA_PATTERN_B;
        const uint8_t *pattern = index == 0 ? m2a_pattern_a : m2a_pattern_b;
        const uint16_t tag = index == 0 ? 0x0102u : 0x0103u;
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_WRITE, 0, tag, lba, 1};
        /* A write response is the header alone: nothing to receive. */
        if (m2a_request(&request, pattern, M2A_HEADER_BYTES, &response, NULL, 0,
                        &length) != 0) {
            m2a_fail(M2A_FAIL_WRITE_SUBMIT);
        }
        if (length != 0) {
            m2a_fail(M2A_FAIL_WRITE_FRAME);
        }
        m2a_expect_header(&response, YAN_OS_BLOCK_OP_WRITE, tag, lba, 1,
                          M2A_FAIL_WRITE_FRAME);
        m2a_puts("m2a: write lba=");
        m2a_put_dec(lba);
        m2a_puts(" bytes=4096 ok\r\n");
    }

    /* Read both written blocks back, plus the block that was never written. */
    for (uint32_t index = 0; index < 3u; ++index) {
        const uint32_t lba = index == 0   ? M2A_LBA_PATTERN_A
                             : index == 1 ? M2A_LBA_PATTERN_B
                                          : M2A_LBA_UNWRITTEN;
        const uint16_t tag = (uint16_t)(0x0140u + index);
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_READ, 0, tag, lba, 1};
        if (m2a_request(&request, NULL, M2A_READ_FRAME, &response, m2a_read_buffer,
                        M2A_BLOCK_BYTES, &length) != 0) {
            m2a_fail(M2A_FAIL_READ_TAKE);
        }
        if (length != M2A_BLOCK_BYTES) {
            m2a_fail(M2A_FAIL_READ_TAKE);
        }
        m2a_expect_header(&response, YAN_OS_BLOCK_OP_READ, tag, lba, 1,
                          M2A_FAIL_READ_TAKE);
        if (lba == M2A_LBA_UNWRITTEN) {
            if (!m2a_all_zero(m2a_read_buffer)) {
                m2a_fail(M2A_FAIL_UNWRITTEN_BLOCK);
            }
            m2a_puts("m2a: read lba=0 zero=1\r\n");
        } else {
            if (memcmp(m2a_read_buffer, m2a_pattern_for(lba), M2A_BLOCK_BYTES) != 0) {
                m2a_fail(M2A_FAIL_PAYLOAD);
            }
            m2a_puts("m2a: read lba=");
            m2a_put_dec(lba);
            m2a_puts(" bytes=4096 match=1\r\n");
        }
    }

    /* ---- the blocked window --------------------------------------------- */
    /* Diagnostic, not an assertion: it counts the rounds the other task ran
     * while this task was still doing the sequential phases above. A wait that
     * never blocks runs no other task, so this reads 0 against the production
     * pump - whose answer is published at the first instruction boundary after
     * the doorbell, before the caller can reach wait. It is printed rather than
     * asserted because a host with real latency would legitimately block here,
     * and that is not a defect of this case. */
    m2a_puts("m2a: sequential-waits idle_ticks=");
    m2a_put_dec(m2a_shared.idle_ticks);
    m2a_puts("\r\n");

    for (uint32_t round = 0; round < M2A_ROUNDS; ++round) {
        m2a_shared.phase = 10u + round;
        const uint32_t window = round == 0 ? M2A_WINDOW_FIRST : M2A_WINDOW_SECOND;
        /* Round 0 reads A then B, round 1 reads B then A: the tags keep the two
         * responses of a round apart, and the payloads prove the response
         * delivered after the wake is the block that was asked for. */
        const uint32_t first_lba = round == 0 ? M2A_LBA_PATTERN_A : M2A_LBA_PATTERN_B;
        const uint32_t second_lba = round == 0 ? M2A_LBA_PATTERN_B : M2A_LBA_PATTERN_A;
        const uint16_t first_tag = (uint16_t)(0x0201u + 0x10u * round);
        const uint16_t second_tag = (uint16_t)(0x0202u + 0x10u * round);

        /* A round starts from a drained stream: every frame of the previous
         * round was taken and every request was consumed. What this asserts is
         * the pairing, not when the pump happens to be triggered - a frame left
         * behind would mean a response nobody took. */
        if (yan_os_transport_h2g_available() != 0) {
            m2a_fail(M2A_FAIL_RING_NOT_EMPTY);
        }
        /* The other task's half of this round, written before `armed` so it
         * cannot see a half-filled handshake. -1 means "not done yet". */
        m2a_shared.drain_tag[round] = (uint32_t)first_tag;
        m2a_shared.drain_lba[round] = first_lba;
        m2a_shared.drain_fail[round] = (uint32_t)-1;

        const YanOsBlockRequest first = {YAN_OS_BLOCK_OP_READ, 0, first_tag, first_lba, 1};
        const YanOsBlockRequest second = {YAN_OS_BLOCK_OP_READ, 0, second_tag, second_lba, 1};
        if (yan_os_block_submit(&first, NULL) != 0) {
            m2a_fail(M2A_FAIL_ROUND_SUBMIT);
        }
        if (yan_os_block_submit(&second, NULL) != 0) {
            m2a_fail(M2A_FAIL_ROUND_SUBMIT);
        }

        /* Arm the window and block. No yield, exit or wait sits between the two
         * stores and this call, so the other task cannot run in between: by the
         * time it sees `armed`, this task is BLOCKED. The predicate asks for the
         * *second* response, which cannot exist yet - the ring holds the first
         * one and R2's 4112 bytes do not fit beside it. */
        m2a_shared.window[round] = window;
        m2a_shared.armed = round + 1u;
        {
            M2aWait wait = {M2A_READ_FRAME, (uint32_t)second_tag};
            const uint32_t polls_before = m2a_shared.polls;
            yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, m2a_frame_ready, &wait);
            const uint32_t polls_delta = m2a_shared.polls - polls_before;
            const uint32_t ticks = m2a_shared.window_ticks[round];

            /* The measurement is printed before it is judged, so a failing run
             * shows what was actually observed rather than only the code. */
            m2a_puts("m2a: window round=");
            m2a_put_dec(round);
            m2a_puts(" budget=");
            m2a_put_dec(window);
            m2a_puts(" ticks=");
            m2a_put_dec(ticks);
            m2a_puts(" polls=");
            m2a_put_dec(polls_delta);
            m2a_puts("\r\n");

            /* The window has to have elapsed, or the wait returned before the
             * event: that would mean this case measured nothing. It cannot
             * elapse early, because the queued reply only becomes possible once
             * the other task has taken the first frame - which it does at the end
             * of the window. */
            if (ticks != window) {
                m2a_fail(M2A_FAIL_NO_WINDOW);
            }
            /* The discriminating assertion. A task that was really blocked is
             * asked exactly once (SPEC step 2) and is not asked again on the
             * way out, so its own activity does not scale with the progress the
             * other task made in the same window. */
            if (polls_delta != 1u) {
                m2a_fail(M2A_FAIL_POLLED_WHILE_BLOCKED);
            }
            /* The wake required the other task to have freed the ring first, so
             * waking with no drain recorded would mean something other than the
             * answer woke this task. */
            if (m2a_shared.drain_fail[round] == (uint32_t)-1) {
                m2a_fail(M2A_FAIL_DRAIN_MISSING);
            }
            if (m2a_shared.drain_fail[round] != 0u) {
                m2a_fail(m2a_shared.drain_fail[round]);
            }
            /* Only os/trap_entry.S's handler can turn a BLOCKED task RUNNABLE,
             * and 0019's ISR order services and acks the device *before* it
             * does: the level is withdrawn by the time this task runs again, so
             * the interrupt status bit is clear. A wake that left the device
             * condition asserted would be the complete-before-service defect. */
            if ((yan_os_transport_irq_status() & YAN_OS_TRANSPORT_IRQ_H2G_DATA) != 0) {
                m2a_fail(M2A_FAIL_ISR_NOT_SERVICED);
            }
        }

        /* The evidence that the frame the other task took is the first one of
         * the pair, and that it did so while this task was blocked. */
        m2a_puts("m2a: drained round=");
        m2a_put_dec(round);
        m2a_puts(" tag=");
        m2a_put_hex(m2a_shared.drain_tag[round]);
        m2a_puts(" lba=");
        m2a_put_dec(m2a_shared.drain_lba[round]);
        m2a_puts(" ok\r\n");

        /* The queued response is the only frame left, and it is the one
         * delivered by the interrupt that woke this task. */
        if (yan_os_block_take(&response, m2a_read_buffer, M2A_BLOCK_BYTES,
                              &length) != 0) {
            m2a_fail(M2A_FAIL_ROUND_TAKE);
        }
        if (length != M2A_BLOCK_BYTES) {
            m2a_fail(M2A_FAIL_ROUND_TAKE);
        }
        m2a_expect_header(&response, YAN_OS_BLOCK_OP_READ, second_tag, second_lba, 1,
                          M2A_FAIL_ROUND_FRAME);
        if (memcmp(m2a_read_buffer, m2a_pattern_for(second_lba), M2A_BLOCK_BYTES) != 0) {
            m2a_fail(M2A_FAIL_PAYLOAD);
        }
        m2a_shared.woken = round + 1u;
    }

    /* ---- the documented trap: take() as the predicate -------------------- */
    m2a_shared.phase = 20;
    {
        uint32_t spins = 0;
        while (m2a_shared.done == 0 && spins < 100000u) {
            ++spins;
            yan_os_task_yield();
        }
        if (m2a_shared.done == 0) {
            m2a_fail(M2A_FAIL_PROGRESS);
        }
    }
    if (yan_os_transport_h2g_available() != 0) {
        m2a_fail(M2A_FAIL_RING_NOT_EMPTY);
    }
    {
        const YanOsBlockRequest request = {YAN_OS_BLOCK_OP_READ, 0, 0x0301u,
                                           M2A_LBA_PATTERN_A, 1};
        if (yan_os_block_submit(&request, NULL) != 0) {
            m2a_fail(M2A_FAIL_ROUND_SUBMIT);
        }
        /* The pump runs between instructions, so a short spin is enough for the
         * response to be in the ring: the predicate must really consume a valid
         * frame, or the case would prove nothing. */
        for (volatile uint32_t spin = 0; spin < 2000u; ++spin) {
        }
        if (yan_os_transport_h2g_available() < M2A_READ_FRAME) {
            m2a_fail(M2A_FAIL_TRAP_SETUP);
        }

        M2aTakePredicate state = {{0, 0, 0, 0, 0}, 0, 0, 0};
        yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, m2a_frame_ready_by_taking, &state);
        if (state.calls != 1u || state.result != 0) {
            m2a_fail(M2A_FAIL_TRAP_PREDICATE);
        }
        /* The frame the predicate consumed was the one this task asked for... */
        if (state.response.op != YAN_OS_BLOCK_OP_READ ||
            state.response.status != YAN_OS_BLOCK_STATUS_OK ||
            state.response.tag != 0x0301u || state.response.lba != M2A_LBA_PATTERN_A ||
            state.response.count != 1u || state.length != M2A_BLOCK_BYTES) {
            m2a_fail(M2A_FAIL_TRAP_PREDICATE);
        }
        /* ...and the caller's own take now answers AGAIN: the response is gone.
         * Only the AGAIN path of take() is free of side effects; the success
         * path consumes, which is exactly why take() cannot be a predicate. */
        if (yan_os_block_take(&response, m2a_read_buffer, M2A_BLOCK_BYTES,
                              &length) != YAN_OS_BLOCK_AGAIN) {
            m2a_fail(M2A_FAIL_TAKE_PREDICATE);
        }
        m2a_puts("m2a: take-predicate calls=");
        m2a_put_dec(state.calls);
        m2a_puts(" caller-take=again\r\n");
    }

    m2a_shared.phase = 30;
    m2a_puts("m2a: PASS\r\n");
    guest_finish(1u);
    for (;;) {
    }
}

/* ==================================================================== main */

/* The IRQ route below the CPU is the caller's business (0019 SPEC): the device's
 * IRQ_ENABLE and the PLIC's enable / priority / threshold. mie.MEIE belongs to
 * the runtime and is not touched here. */
static void m2a_configure_route(void)
{
    yan_os_transport_set_irq_enable(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_set_threshold(0);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
}

int main(void)
{
    m2a_puts("m2a: block I/O through the cooperative runtime\r\n");
    /* Without --disk the channel has no ring and no notify callback, so
     * HOST_READY is 0 and the Guest must not touch the rings (0014). Failing
     * here names the missing host pump instead of timing out. */
    if (!yan_os_transport_host_ready()) {
        m2a_fail(M2A_FAIL_NO_CHANNEL);
    }
    /* 0018's ring floor: RING_SIZE 8192 carries one block, and this case is
     * written against that geometry (two 4112-byte read responses do not fit in
     * the 8191-byte ring, which is the flow control the window is built on). */
    if (yan_os_block_max_count() != 1u) {
        m2a_fail(M2A_FAIL_MAX_COUNT);
    }
    m2a_configure_route();
    if (yan_os_task_spawn(m2a_io_task, NULL) != YAN_OS_TASK_OK ||
        yan_os_task_spawn(m2a_progress_task, NULL) != YAN_OS_TASK_OK) {
        m2a_fail(M2A_FAIL_SPAWN);
    }
    yan_os_sched_run();
    m2a_fail(M2A_FAIL_PROGRESS); /* the scheduler never returns */
}
