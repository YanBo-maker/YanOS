#include "line.h"
#include "unity.h"

#include <string.h>

/* Native tests for the 0022 line reader (os/line.c).
 *
 * The reader is pure C17 and device-agnostic, so the whole machine side is a
 * fake byte stream injected through YanLineIo. The fake models the UART's
 * latched single byte: bytes become "visible" either up front (the fast path)
 * or one per wait (the blocking path), and every callback is counted and
 * logged so the ordering rules can be asserted directly.
 *
 * What this file proves is the reader's own logic and its callback contract:
 * the exact grammar, reject-and-drain, the mask/take/re-arm order, no empty
 * read, no wait without a connection, echo-failure stop, reentrancy and the
 * init guards. It does not exercise the real UART, PLIC or task runtime; those
 * are verified by the Guest tests, which this file deliberately does not
 * pretend to replace. */

#define STREAM_MAX 4096u
#define ECHO_MAX 8192u
#define LOG_MAX 8192u

typedef struct {
    /* Byte source. `visible` bytes are latched in the device; the rest arrive
     * later (one per wait when wait_feeds is set). */
    uint8_t stream[STREAM_MAX];
    uint32_t stream_length;
    uint32_t consumed;
    uint32_t visible;
    bool connected;
    bool wait_feeds;
    uint32_t disconnect_after_waits; /* 0 = never */

    /* Echo sink. */
    uint8_t echo[ECHO_MAX];
    uint32_t echo_length;
    uint32_t put_calls;
    uint32_t put_fail_at; /* 1-based; 0 = never */
    bool put_failed;

    uint32_t connected_calls;
    uint32_t ready_calls;
    uint32_t get_calls;
    uint32_t empty_get_calls;
    /* 1-based get call that answers 0 even though ready said yes (0 = never):
     * the test-only switch for the ready-true/get-zero window. */
    uint32_t force_empty_get_at;
    uint32_t arm_enable_calls;
    uint32_t arm_disable_calls;
    uint32_t ack_calls;
    uint32_t wait_calls;
    uint32_t predicate_calls;
    int predicate_result;
    bool predicate_consumed;
    uint32_t predicate_get_before;
    uint32_t predicate_consumed_before;

    char log[LOG_MAX];
    uint32_t log_length;

    /* Reentry probes: a nested call made from inside wait. */
    YanLine *reentry_line;
    uint32_t reentry_next_at_wait; /* 0 = never */
    bool reentry_next_done;
    YanLineResult reentry_next_result;
    bool reentry_init_at_wait;
    bool reentry_init_done;
    YanLineResult reentry_init_result;
} FakeIo;

/* The line is wrapped so a guard right after it catches a write past
 * buffer[1023]; the reader promises the buffer is exactly 1024 bytes. */
typedef struct {
    YanLine line;
    uint8_t guard[32];
} GuardedLine;

static FakeIo fake;
static GuardedLine holder;

static void log_op(FakeIo *io, char op)
{
    if (io->log_length < LOG_MAX - 1u) {
        io->log[io->log_length] = op;
        ++io->log_length;
        io->log[io->log_length] = '\0';
    }
}

static int fake_connected(void *context)
{
    FakeIo *io = (FakeIo *)context;
    ++io->connected_calls;
    return io->connected ? 1 : 0;
}

static int fake_ready(void *context)
{
    FakeIo *io = (FakeIo *)context;
    ++io->ready_calls;
    return (io->connected && io->consumed < io->visible) ? 1 : 0;
}

static int fake_get_byte(void *context, uint8_t *byte)
{
    FakeIo *io = (FakeIo *)context;
    ++io->get_calls;
    log_op(io, 'G');
    if (io->force_empty_get_at != 0u &&
        io->get_calls == io->force_empty_get_at) {
        ++io->empty_get_calls;
        return 0;
    }
    if (io->consumed >= io->visible) {
        ++io->empty_get_calls;
        return 0;
    }
    *byte = io->stream[io->consumed];
    ++io->consumed;
    return 1;
}

static bool fake_put_byte(void *context, uint8_t byte)
{
    FakeIo *io = (FakeIo *)context;
    ++io->put_calls;
    log_op(io, 'P');
    if (io->put_fail_at != 0u && io->put_calls == io->put_fail_at) {
        io->put_failed = true;
        return false;
    }
    if (io->echo_length < ECHO_MAX) {
        io->echo[io->echo_length] = byte;
        ++io->echo_length;
    }
    return true;
}

static void fake_arm_rx(void *context, bool enable)
{
    FakeIo *io = (FakeIo *)context;
    if (enable) {
        ++io->arm_enable_calls;
        log_op(io, 'A');
    } else {
        ++io->arm_disable_calls;
        log_op(io, 'M');
    }
}

static void fake_ack_rx(void *context)
{
    FakeIo *io = (FakeIo *)context;
    ++io->ack_calls;
    log_op(io, 'K');
}

static void fake_wait(void *context, int (*predicate)(void *),
                      void *predicate_context);
static YanLineIo fake_io(FakeIo *io);

static void fake_wait(void *context, int (*predicate)(void *),
                      void *predicate_context)
{
    FakeIo *io = (FakeIo *)context;
    ++io->wait_calls;
    log_op(io, 'W');
    if (io->reentry_line != NULL && io->reentry_next_at_wait != 0u &&
        io->wait_calls == io->reentry_next_at_wait && !io->reentry_next_done) {
        io->reentry_next_done = true;
        io->reentry_next_result = yan_line_next(io->reentry_line);
    }
    if (io->reentry_line != NULL && io->reentry_init_at_wait &&
        !io->reentry_init_done) {
        io->reentry_init_done = true;
        io->reentry_init_result = yan_line_init(io->reentry_line, fake_io(io));
    }
    /* The predicate runs the way the runtime runs it: read-only, and it must
     * not consume. Record whether it did. */
    io->predicate_get_before = io->get_calls;
    io->predicate_consumed_before = io->consumed;
    ++io->predicate_calls;
    io->predicate_result = predicate(predicate_context);
    io->predicate_consumed = (io->get_calls != io->predicate_get_before) ||
                             (io->consumed != io->predicate_consumed_before);
    /* The event happens after the predicate has looked (the interesting
     * window): make one more byte visible. No loop here. */
    if (io->wait_feeds && io->visible < io->stream_length) {
        ++io->visible;
    }
    if (io->disconnect_after_waits != 0u &&
        io->wait_calls >= io->disconnect_after_waits) {
        io->connected = false;
    }
}

static YanLineIo fake_io(FakeIo *io)
{
    YanLineIo result;
    result.context = io;
    result.connected = fake_connected;
    result.ready = fake_ready;
    result.get_byte = fake_get_byte;
    result.put_byte = fake_put_byte;
    result.arm_rx = fake_arm_rx;
    result.ack_rx = fake_ack_rx;
    result.wait = fake_wait;
    return result;
}

/* ------------------------------------------------------------ test helpers */

static void load_stream(const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_TRUE(length <= STREAM_MAX);
    if (length > 0u) {
        memcpy(fake.stream, bytes, length);
    }
    fake.stream_length = length;
    fake.consumed = 0u;
    fake.visible = length; /* fast path unless a test opts into the wait path */
}

static void load_text(const char *text)
{
    load_stream((const uint8_t *)text, (uint32_t)strlen(text));
}

static void make_wait_path(void)
{
    fake.visible = 0u;
    fake.wait_feeds = true;
}

static void setup_line(void)
{
    memset(&fake, 0, sizeof fake);
    fake.connected = true;
    memset(&holder, 0, sizeof holder);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_init(&holder.line, fake_io(&fake)));
}

static void expect_echo_bytes(const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_UINT32(length, fake.echo_length);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY(bytes, fake.echo, length);
    }
}

static void expect_echo(const char *text)
{
    expect_echo_bytes((const uint8_t *)text, (uint32_t)strlen(text));
}

static void expect_log(const char *text)
{
    TEST_ASSERT_EQUAL_STRING(text, fake.log);
}

static void assert_mask_before_every_get(void)
{
    for (uint32_t i = 0; i < fake.log_length; ++i) {
        if (fake.log[i] == 'G') {
            TEST_ASSERT_TRUE(i > 0u);
            TEST_ASSERT_EQUAL_INT('M', (int)fake.log[i - 1u]);
        }
    }
}

/* One mask per take, plus one release mask; one re-arm per take except the
 * last, plus one per wait. */
static void assert_arm_arithmetic(void)
{
    uint32_t expected_rearm = fake.wait_calls;
    if (fake.get_calls > 0u) {
        expected_rearm += fake.get_calls - 1u;
    }
    TEST_ASSERT_EQUAL_UINT32(expected_rearm, fake.arm_enable_calls);
    TEST_ASSERT_EQUAL_UINT32(fake.get_calls + fake.ack_calls, fake.arm_disable_calls);
}

static uint8_t big[STREAM_MAX];

void setUp(void)
{
    setup_line();
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------ basics */

static void empty_line_returns_ok_with_zero_length(void)
{
    load_text("\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);
    expect_echo("\r\n");
    expect_log("MGPPMK");
    TEST_ASSERT_EQUAL_UINT32(1u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.empty_get_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.ack_calls);
    assert_mask_before_every_get();
    assert_arm_arithmetic();
}

static void standalone_lf_ends_a_line(void)
{
    load_text("hi\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("hi", holder.line.buffer, 2u);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[2]);
    expect_echo("hi\r\n");
}

static void cr_only_line_returns_empty_and_swallows_a_following_lf(void)
{
    static const uint8_t input[] = {'\r', '\n', 'x', '\n'};
    load_stream(input, sizeof input);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(1u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'x', holder.line.buffer[0]);
}

static void crlf_pair_produces_one_line_and_lf_is_swallowed_next_call(void)
{
    load_text("abc\r\ndef\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(3u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("abc", holder.line.buffer, 3u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(3u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("def", holder.line.buffer, 3u);
}

static void cr_without_lf_preserves_the_following_byte(void)
{
    load_text("a\rb\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(1u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'a', holder.line.buffer[0]);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(1u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'b', holder.line.buffer[0]);
}

static void backspace_deletes_the_last_byte_and_echoes_the_erase(void)
{
    static const uint8_t input[] = {'a', 'b', 0x08u, 'c', '\n'};
    load_stream(input, sizeof input);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("ac", holder.line.buffer, 2u);
    static const uint8_t expected_echo[] = {
        'a', 'b', 0x08u, ' ', 0x08u, 'c', '\r', '\n'
    };
    expect_echo_bytes(expected_echo, sizeof expected_echo);
}

static void backspace_on_an_empty_line_is_silent(void)
{
    static const uint8_t input[] = {0x08u, '\n'};
    load_stream(input, sizeof input);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    expect_echo_bytes((const uint8_t *)"\r\n", 2u);
}

static void high_bytes_are_stored_and_echoed_unchanged(void)
{
    static const uint8_t input[] = {0xc3u, 0xa9u, '\n'};
    load_stream(input, sizeof input);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0xc3u, holder.line.buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0xa9u, holder.line.buffer[1]);
    static const uint8_t expected_echo[] = {0xc3u, 0xa9u, '\r', '\n'};
    expect_echo_bytes(expected_echo, sizeof expected_echo);
}

static void accepted_line_has_a_nul_terminator(void)
{
    load_text("hi\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[2]);
}

/* ---------------------------------------------------------- invalid input */

static void nul_tab_escape_and_other_c0_reject_the_line(void)
{
    static const uint8_t controls[] = {0x00u, 0x01u, 0x09u, 0x1bu, 0x1fu};
    for (uint32_t i = 0; i < sizeof controls; ++i) {
        setup_line();
        uint8_t input[4];
        input[0] = (uint8_t)'a';
        input[1] = controls[i];
        input[2] = (uint8_t)'b';
        input[3] = (uint8_t)'\n';
        load_stream(input, sizeof input);
        TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID_INPUT, yan_line_next(&holder.line));
        TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
        TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);
        /* 'a' was echoed before the control; the control and 'b' were not. */
        TEST_ASSERT_EQUAL_UINT32(3u, fake.echo_length);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)'a', fake.echo[0]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)'\r', fake.echo[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)'\n', fake.echo[2]);
        TEST_ASSERT_EQUAL_UINT32(4u, fake.get_calls);
        TEST_ASSERT_EQUAL_UINT32(1u, fake.ack_calls);
    }
}

static void control_bytes_are_not_spliced_into_a_command(void)
{
    static const uint8_t input[] = {
        'c', 'r', 'e', 0x1bu, 'a', 't', 'e', ' ', 'a', '\n'
    };
    load_stream(input, sizeof input);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID_INPUT, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);
}

/* ------------------------------------------------------------- line limits */

static void line_of_1023_bytes_is_accepted(void)
{
    memset(big, 'a', 1023u);
    big[1023] = (uint8_t)'\n';
    load_stream(big, 1024u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(1023u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[1023]);
    for (uint32_t i = 0; i < 1023u; ++i) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)'a', holder.line.buffer[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(1025u, fake.echo_length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\r', fake.echo[1023]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\n', fake.echo[1024]);
}

static void line_of_1024_bytes_is_too_long(void)
{
    memset(big, 'a', 1024u);
    big[1024] = (uint8_t)'\n';
    load_stream(big, 1025u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_TOO_LONG, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);
    /* The first 1023 bytes were echoed; the 1024th and the drain were not. */
    TEST_ASSERT_EQUAL_UINT32(1025u, fake.echo_length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\r', fake.echo[1023]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\n', fake.echo[1024]);
    TEST_ASSERT_EQUAL_UINT32(1025u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.empty_get_calls);
}

static void too_long_line_drains_to_the_end_without_echo(void)
{
    memset(big, 'a', 2048u);
    big[2048] = (uint8_t)'\n';
    load_stream(big, 2049u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_TOO_LONG, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2049u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(1025u, fake.echo_length);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.empty_get_calls);
    assert_mask_before_every_get();
    assert_arm_arithmetic();
}

static void too_long_line_ignores_backspace_and_later_bytes(void)
{
    memset(big, 'a', 1024u);
    big[1024] = 0x08u;
    big[1025] = (uint8_t)'x';
    big[1026] = (uint8_t)'y';
    big[1027] = (uint8_t)'z';
    big[1028] = (uint8_t)'\n';
    load_stream(big, 1029u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_TOO_LONG, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    /* Only the accepted prefix and the line ending were echoed. */
    TEST_ASSERT_EQUAL_UINT32(1025u, fake.echo_length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\r', fake.echo[1023]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\n', fake.echo[1024]);
}

static void too_long_line_does_not_write_past_the_buffer(void)
{
    memset(holder.guard, 0xa5u, sizeof holder.guard);
    memset(big, 'a', 2048u);
    big[2048] = (uint8_t)'\n';
    load_stream(big, 2049u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_TOO_LONG, yan_line_next(&holder.line));
    for (uint32_t i = 0; i < sizeof holder.guard; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0xa5u, holder.guard[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);

    setup_line();
    memset(holder.guard, 0xa5u, sizeof holder.guard);
    memset(big, 'a', 1023u);
    big[1023] = (uint8_t)'\n';
    load_stream(big, 1024u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    for (uint32_t i = 0; i < sizeof holder.guard; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0xa5u, holder.guard[i]);
    }
}

static void first_reject_reason_wins(void)
{
    memset(big, 'a', 1024u);
    big[1024] = 0x1bu;
    big[1025] = (uint8_t)'\n';
    load_stream(big, 1026u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_TOO_LONG, yan_line_next(&holder.line));

    setup_line();
    big[0] = 0x1bu;
    memset(big + 1u, 'a', 1024u);
    big[1025] = (uint8_t)'\n';
    load_stream(big, 1026u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID_INPUT, yan_line_next(&holder.line));
}

/* ----------------------------------------------------------- wait and race */

static void ready_path_never_waits(void)
{
    load_text("ab\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, fake.wait_calls);
    TEST_ASSERT_EQUAL_UINT32(3u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.empty_get_calls);
    TEST_ASSERT_EQUAL_UINT32(2u, fake.arm_enable_calls);
    TEST_ASSERT_EQUAL_UINT32(4u, fake.arm_disable_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.ack_calls);
    expect_log("MGPAMGPAMGPPMK");
    assert_mask_before_every_get();
    assert_arm_arithmetic();
}

static void each_byte_is_masked_before_it_is_taken_and_rearmed_after(void)
{
    load_text("ab\n");
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    /* M mask, G take, P echo, A re-arm; the ending takes without a re-arm and
     * then the release masks and acknowledges: M G P A M G P A M G P P M K. */
    expect_log("MGPAMGPAMGPPMK");
    assert_mask_before_every_get();
    assert_arm_arithmetic();
}

static void wait_path_delivers_bytes_without_empty_reads(void)
{
    load_text("xy\n");
    make_wait_path();
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(3u, fake.wait_calls);
    TEST_ASSERT_EQUAL_UINT32(3u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.empty_get_calls);
    TEST_ASSERT_EQUAL_UINT32(3u, fake.predicate_calls);
    TEST_ASSERT_FALSE(fake.predicate_consumed);
    assert_mask_before_every_get();
    assert_arm_arithmetic();
}

static void predicate_is_read_only_and_does_not_consume(void)
{
    load_text("\n");
    make_wait_path();
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(1u, fake.predicate_calls);
    TEST_ASSERT_FALSE(fake.predicate_consumed);
    /* The predicate looked before the byte became visible, so it saw "not
     * ready"; the byte was still there afterwards. */
    TEST_ASSERT_EQUAL_INT(0, fake.predicate_result);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.get_calls);
}

static void disconnected_terminal_returns_unavailable_without_waiting(void)
{
    load_text("\n");
    fake.connected = false;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_UNAVAILABLE, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, fake.wait_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.arm_disable_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.ack_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
}

static void disconnect_during_wait_returns_unavailable(void)
{
    load_text("ab\n");
    make_wait_path();
    fake.disconnect_after_waits = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_UNAVAILABLE, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(1u, fake.wait_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.get_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
}

/* ----------------------------------------------------------- echo failure */

static void echo_failure_on_the_first_byte_stops_immediately(void)
{
    load_text("a\n");
    fake.put_fail_at = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_UNAVAILABLE, yan_line_next(&holder.line));
    TEST_ASSERT_TRUE(fake.put_failed);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.put_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.echo_length);
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
}

static void echo_failure_in_the_middle_stops_immediately(void)
{
    load_text("abc\n");
    fake.put_fail_at = 2u;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_UNAVAILABLE, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, fake.put_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.echo_length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'a', fake.echo[0]);
}

static void echo_failure_on_the_line_ending_stops_immediately(void)
{
    load_text("a\n");
    fake.put_fail_at = 2u; /* the CR of the "\r\n" ending */
    TEST_ASSERT_EQUAL_INT(YAN_LINE_UNAVAILABLE, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, fake.put_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.echo_length);
}

/* -------------------------------------------------------------- reentrancy */

static void reentrant_next_returns_busy_and_does_not_touch_the_device(void)
{
    load_text("ab\n");
    make_wait_path();
    fake.reentry_line = &holder.line;
    fake.reentry_next_at_wait = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_TRUE(fake.reentry_next_done);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_BUSY, fake.reentry_next_result);
    TEST_ASSERT_EQUAL_UINT32(3u, fake.get_calls);
    TEST_ASSERT_FALSE(fake.predicate_consumed);
}

static void reentrant_init_while_busy_returns_busy(void)
{
    load_text("ab\n");
    make_wait_path();
    fake.reentry_line = &holder.line;
    fake.reentry_init_at_wait = true;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_TRUE(fake.reentry_init_done);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_BUSY, fake.reentry_init_result);
}

/* ------------------------------------------------------------ init guards */

static void init_validates_every_callback(void)
{
    YanLine candidate;
    memset(&candidate, 0, sizeof candidate);
    YanLineIo io = fake_io(&fake);

    io.connected = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    io = fake_io(&fake);
    io.ready = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    io = fake_io(&fake);
    io.get_byte = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    io = fake_io(&fake);
    io.put_byte = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    io = fake_io(&fake);
    io.arm_rx = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    io = fake_io(&fake);
    io.ack_rx = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    io = fake_io(&fake);
    io.wait = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    TEST_ASSERT_FALSE(candidate.initialized);
}

static void init_rejects_a_null_line(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(NULL, fake_io(&fake)));
}

/* The uintptr range check runs before any initialized/busy read, so a pointer
 * whose object would leave the address space is refused without a load. */
static void init_rejects_a_line_pointer_whose_object_leaves_uintptr(void)
{
    YanLine *wild = (YanLine *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)3u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(wild, fake_io(&fake)));
}

/* API-level refusals leave the object exactly as it was; only a line that was
 * actually read and then rejected is emptied. */
static void invalid_and_busy_refusals_leave_the_line_unchanged(void)
{
    YanLine candidate;
    memset(&candidate, 0, sizeof candidate);
    candidate.length = 7u;
    candidate.buffer[0] = (uint8_t)'x';
    unsigned char before[sizeof candidate];
    memcpy(before, &candidate, sizeof candidate);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_next(&candidate));
    TEST_ASSERT_EQUAL_MEMORY(before, &candidate, sizeof candidate);

    holder.line.busy = true;
    holder.line.length = 5u;
    holder.line.buffer[0] = (uint8_t)'z';
    unsigned char held[sizeof holder.line];
    memcpy(held, &holder.line, sizeof holder.line);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_BUSY, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_MEMORY(held, &holder.line, sizeof holder.line);
    holder.line.busy = false;
}

static void init_may_reinitialize_an_idle_line(void)
{
    /* setUp already initialized holder.line; a second init on an idle instance
     * is allowed and resets the reader state, which is what a terminal reopen
     * relies on. */
    holder.line.swallow_lf = true;
    holder.line.length = 7u;
    holder.line.buffer[0] = (uint8_t)'x';
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_init(&holder.line, fake_io(&fake)));
    TEST_ASSERT_TRUE(holder.line.initialized);
    TEST_ASSERT_FALSE(holder.line.busy);
    TEST_ASSERT_FALSE(holder.line.swallow_lf);
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);
}

static void init_rejects_a_context_that_aliases_the_line(void)
{
    YanLine candidate;
    unsigned char before[sizeof candidate];
    memset(&candidate, 0, sizeof candidate);
    candidate.length = 0x12345678u;
    candidate.buffer[0] = 0xa5u;
    memcpy(before, &candidate, sizeof candidate);

    YanLineIo io = fake_io(&fake);
    io.context = candidate.buffer;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    TEST_ASSERT_EQUAL_MEMORY(before, &candidate, sizeof candidate);

    io.context = &candidate;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_init(&candidate, io));
    TEST_ASSERT_EQUAL_MEMORY(before, &candidate, sizeof candidate);
}

static void next_before_init_returns_invalid(void)
{
    YanLine candidate;
    memset(&candidate, 0, sizeof candidate);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_next(&candidate));
}

static void next_on_a_null_line_returns_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID, yan_line_next(NULL));
}

/* -------------------------------------------------- recovery between lines */

static void next_line_after_an_invalid_input_line_is_clean(void)
{
    static const uint8_t input[] = {'a', 0x1bu, 'b', '\n', 'o', 'k', '\n'};
    load_stream(input, sizeof input);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID_INPUT, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("ok", holder.line.buffer, 2u);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[2]);
}

static void next_line_after_a_too_long_line_is_clean(void)
{
    memset(big, 'a', 1024u);
    big[1024] = (uint8_t)'\n';
    big[1025] = (uint8_t)'o';
    big[1026] = (uint8_t)'k';
    big[1027] = (uint8_t)'\n';
    load_stream(big, 1028u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_TOO_LONG, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("ok", holder.line.buffer, 2u);
}

/* The device reported ready, then get_byte answered 0 (a spurious ready). The
 * reader must not treat that as a byte: it re-arms this UART's interrupt and
 * looks again, so the same call still returns the real line with no wait and no
 * lost input. The forced-empty switch is test-only. */
static void ready_true_get_zero_rearms_then_reads(void)
{
    load_text("ab\n");
    fake.force_empty_get_at = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("ab", holder.line.buffer, 2u);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[2]);
    expect_echo("ab\r\n");
    TEST_ASSERT_EQUAL_UINT32(1u, fake.empty_get_calls);
    TEST_ASSERT_EQUAL_UINT32(4u, fake.get_calls); /* one empty, three real */
    /* mask, take (0), re-arm, take 'a', echo, re-arm, take 'b', ... */
    /* Empty take re-arms; 'a' and 'b' each echo then re-arm; LF echoes
     * both CR and LF before the final mask and acknowledgement. */
    TEST_ASSERT_EQUAL_STRING("MGAMGPAMGPAMGPPMK", fake.log);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.ack_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.wait_calls); /* the ready path never waits */
    assert_mask_before_every_get();
    assert_arm_arithmetic();
}

/* 0022 control bytes reject the whole line and are never appended. A control
 * byte after a full 1023-byte line is checked before the length rule, so the
 * reason is INVALID_INPUT, the control stays out of the buffer, and the next
 * line is clean. (The other order - the 1024th data byte before a later control
 * - keeps TOO_LONG as the first reason; first_reject_reason_wins covers it.) */
static void control_after_1023_bytes_keeps_the_control_reason(void)
{
    memset(big, 'a', 1023u);
    big[1023] = 0x1bu; /* ESC, not appended */
    big[1024] = (uint8_t)'\n';
    big[1025] = (uint8_t)'o';
    big[1026] = (uint8_t)'k';
    big[1027] = (uint8_t)'\n';
    load_stream(big, 1028u);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_INVALID_INPUT, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(0u, holder.line.length);
    TEST_ASSERT_EQUAL_UINT8(0u, holder.line.buffer[0]);
    TEST_ASSERT_EQUAL_INT(YAN_LINE_OK, yan_line_next(&holder.line));
    TEST_ASSERT_EQUAL_UINT32(2u, holder.line.length);
    TEST_ASSERT_EQUAL_MEMORY("ok", holder.line.buffer, 2u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(empty_line_returns_ok_with_zero_length);
    RUN_TEST(standalone_lf_ends_a_line);
    RUN_TEST(cr_only_line_returns_empty_and_swallows_a_following_lf);
    RUN_TEST(crlf_pair_produces_one_line_and_lf_is_swallowed_next_call);
    RUN_TEST(cr_without_lf_preserves_the_following_byte);
    RUN_TEST(backspace_deletes_the_last_byte_and_echoes_the_erase);
    RUN_TEST(backspace_on_an_empty_line_is_silent);
    RUN_TEST(high_bytes_are_stored_and_echoed_unchanged);
    RUN_TEST(accepted_line_has_a_nul_terminator);
    RUN_TEST(nul_tab_escape_and_other_c0_reject_the_line);
    RUN_TEST(control_bytes_are_not_spliced_into_a_command);
    RUN_TEST(line_of_1023_bytes_is_accepted);
    RUN_TEST(line_of_1024_bytes_is_too_long);
    RUN_TEST(too_long_line_drains_to_the_end_without_echo);
    RUN_TEST(too_long_line_ignores_backspace_and_later_bytes);
    RUN_TEST(too_long_line_does_not_write_past_the_buffer);
    RUN_TEST(first_reject_reason_wins);
    RUN_TEST(ready_path_never_waits);
    RUN_TEST(each_byte_is_masked_before_it_is_taken_and_rearmed_after);
    RUN_TEST(wait_path_delivers_bytes_without_empty_reads);
    RUN_TEST(predicate_is_read_only_and_does_not_consume);
    RUN_TEST(disconnected_terminal_returns_unavailable_without_waiting);
    RUN_TEST(disconnect_during_wait_returns_unavailable);
    RUN_TEST(echo_failure_on_the_first_byte_stops_immediately);
    RUN_TEST(echo_failure_in_the_middle_stops_immediately);
    RUN_TEST(echo_failure_on_the_line_ending_stops_immediately);
    RUN_TEST(reentrant_next_returns_busy_and_does_not_touch_the_device);
    RUN_TEST(reentrant_init_while_busy_returns_busy);
    RUN_TEST(init_validates_every_callback);
    RUN_TEST(init_rejects_a_null_line);
    RUN_TEST(init_rejects_a_line_pointer_whose_object_leaves_uintptr);
    RUN_TEST(init_may_reinitialize_an_idle_line);
    RUN_TEST(init_rejects_a_context_that_aliases_the_line);
    RUN_TEST(invalid_and_busy_refusals_leave_the_line_unchanged);
    RUN_TEST(next_before_init_returns_invalid);
    RUN_TEST(next_on_a_null_line_returns_invalid);
    RUN_TEST(next_line_after_an_invalid_input_line_is_clean);
    RUN_TEST(next_line_after_a_too_long_line_is_clean);
    RUN_TEST(ready_true_get_zero_rearms_then_reads);
    RUN_TEST(control_after_1023_bytes_keeps_the_control_reason);
    return UNITY_END();
}
