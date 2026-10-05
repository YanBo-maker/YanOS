/* Guest self-check for the YanOS console driver, os/console.c.
 *
 * The program is the executable form of docs/specs/0017-console-and-os-layout.md
 * section B: every check below asserts one row of the spec's behaviour tables by
 * calling the driver's four entry points and inspecting what came back. A failed
 * check ends the run with `tohost` set to the check number, so the runner
 * reports exactly which expectation broke; the same convention the Guest corpus
 * and tests/guest/trap_env.c use.
 *
 * What the Guest cannot see is asserted by the runner instead, because delivery
 * and connection state belong to the Host end of the device:
 *
 *   - bytes the driver handed to TXDATA  (the runner captures them),
 *   - whether TXDATA accepts a byte      (the runner's backend decides),
 *   - when the terminal disappears       (the runner detaches it).
 *
 * The Guest therefore asserts return codes, buffer contents and its own view of
 * `yan_os_console_connected()`; tests/guest/run_console.sh asserts the captured
 * byte stream and lines the two layers up with the B1 - B8 numbering used here.
 *
 * One scenario is compiled in per image (`-DYAN_CONSOLE_SCENARIO=...`) because
 * each one needs a different terminal behaviour, which the runner scripts. All
 * scenarios run the invalid-argument checks first: an invalid call is invalid
 * whether or not a terminal is attached, so the driver must reject it before it
 * looks at the device.
 */
#include "console.h"

#include "guest.h"

#define YAN_CONSOLE_SCENARIO_HEADLESS 1
#define YAN_CONSOLE_SCENARIO_OUTPUT 2
#define YAN_CONSOLE_SCENARIO_REFUSED 3
#define YAN_CONSOLE_SCENARIO_EDIT 4
#define YAN_CONSOLE_SCENARIO_BOUNDARY 5
#define YAN_CONSOLE_SCENARIO_CRLF 6
#define YAN_CONSOLE_SCENARIO_DISCONNECT 7
#define YAN_CONSOLE_SCENARIO_HIGH_BYTE 8
#define YAN_CONSOLE_SCENARIO_ECHO_REFUSED 9
#define YAN_CONSOLE_SCENARIO_ECHO_REFUSED_BACKSPACE 10
#define YAN_CONSOLE_SCENARIO_ECHO_REFUSED_LINE_END 11

#ifndef YAN_CONSOLE_SCENARIO
#define YAN_CONSOLE_SCENARIO YAN_CONSOLE_SCENARIO_HEADLESS
#endif

/* B8: invalid arguments, checked in every scenario. The codes start at 2
 * because tohost == 1 is the pass code the runners read: a check numbered 1
 * would report a failure as a pass. */
#define CHECK_B8_PUTS_NULL 2
#define CHECK_B8_BUFFER_NULL 3
#define CHECK_B8_CAPACITY_ZERO 4
#define CHECK_B8_LENGTH_NULL 5
#define CHECK_B8_LENGTH_UNTOUCHED 6
#define CHECK_B8_BUFFER_UNTOUCHED 7

/* B1: no terminal attached. */
#define CHECK_B1_NOT_CONNECTED 10
#define CHECK_B1_PUTC 11
#define CHECK_B1_PUTS 12
#define CHECK_B1_PUTS_EMPTY 13
#define CHECK_B1_GETLINE 14
#define CHECK_B1_LENGTH_UNTOUCHED 15
#define CHECK_B1_BUFFER_UNTOUCHED 16
#define CHECK_B1_REPEATED 17

/* B2: output arrives byte for byte. */
#define CHECK_B2_CONNECTED 20
#define CHECK_B2_PUTS_OK 21
#define CHECK_B2_PUTS_EMPTY_OK 22

/* B3: the device refuses a byte. */
#define CHECK_B3_CONNECTED 30
#define CHECK_B3_PUTS_OK 31
#define CHECK_B3_PUTC_REFUSED 32
#define CHECK_B3_PUTS_STOPS 33

/* B4: line editing. */
#define CHECK_B4_CONNECTED 40
#define CHECK_B4_RESULT 41
#define CHECK_B4_LENGTH 42
#define CHECK_B4_TEXT 43

/* B5: buffer boundary, including the sentinel bytes around it. */
#define CHECK_B5_CONNECTED 50
#define CHECK_B5_RESULT 51
#define CHECK_B5_LENGTH 52
#define CHECK_B5_TEXT 53
#define CHECK_B5_TERMINATOR 54
#define CHECK_B5_SENTINEL_BEFORE 55
#define CHECK_B5_SENTINEL_AFTER 56

/* B6: CRLF is one line ending. */
#define CHECK_B6_CONNECTED 60
#define CHECK_B6_FIRST_RESULT 61
#define CHECK_B6_FIRST_LENGTH 62
#define CHECK_B6_FIRST_TEXT 63
#define CHECK_B6_SECOND_RESULT 64
#define CHECK_B6_SECOND_LENGTH 65
#define CHECK_B6_SECOND_TEXT 66

/* B7: the terminal disappears while getline waits. */
#define CHECK_B7_CONNECTED 70
#define CHECK_B7_RESULT 71
#define CHECK_B7_LENGTH_UNTOUCHED 72
#define CHECK_B7_DISCONNECTED 73

/* Beyond B1 - B8: a byte outside 7-bit ASCII is passed through unchanged, as
 * the spec's "out of scope" note requires for non-ASCII input. Kept as its own
 * scenario so a change of interpretation touches one place. */
#define CHECK_HIGH_CONNECTED 80
#define CHECK_HIGH_RESULT 81
#define CHECK_HIGH_LENGTH 82
#define CHECK_HIGH_BYTE 83
#define CHECK_HIGH_TERMINATOR 84

/* Beyond B1 - B8: the echo is refused. getline writes its echo itself, and the
 * driver's documented answer to a refused echo is YAN_OS_UNAVAILABLE with
 * *length left unwritten and the buffer still terminated. The three echo call
 * sites are separate code, so each gets its own scenario. The backend refuses
 * from a fixed delivered-byte count on, which is the only way to observe
 * CONNECTED = 1 together with TX_READY = 0 at that exact moment. */
#define CHECK_ECHO_REFUSED_CONNECTED 90
#define CHECK_ECHO_REFUSED_RESULT 91
#define CHECK_ECHO_REFUSED_LENGTH_UNTOUCHED 92
#define CHECK_ECHO_REFUSED_TEXT 93
#define CHECK_ECHO_REFUSED_SENTINEL_BEFORE 94
#define CHECK_ECHO_REFUSED_SENTINEL_AFTER 95

#define CHECK_ECHO_BACKSPACE_CONNECTED 100
#define CHECK_ECHO_BACKSPACE_RESULT 101
#define CHECK_ECHO_BACKSPACE_LENGTH_UNTOUCHED 102
#define CHECK_ECHO_BACKSPACE_TEXT 103
#define CHECK_ECHO_BACKSPACE_SENTINEL_BEFORE 104
#define CHECK_ECHO_BACKSPACE_SENTINEL_AFTER 105

#define CHECK_ECHO_LINE_END_CONNECTED 110
#define CHECK_ECHO_LINE_END_RESULT 111
#define CHECK_ECHO_LINE_END_LENGTH_UNTOUCHED 112
#define CHECK_ECHO_LINE_END_TEXT 113
#define CHECK_ECHO_LINE_END_SENTINEL_BEFORE 114
#define CHECK_ECHO_LINE_END_SENTINEL_AFTER 115

#define CONSOLE_LINE_CAPACITY 16

/* Sentinel values: the buffer must look untouched after a call that reports
 * "nothing happened", so every check starts from a known pattern. */
#define UNWRITTEN UINT32_C(0xdeadbeef)
#define SENTINEL_BYTE 0xaa

static char line[CONSOLE_LINE_CAPACITY];
static unsigned length;

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_BOUNDARY
/* capacity 5 inside a 9-byte area: indices 0-1 and 7-8 are sentinels the driver
 * must not write through, which is how B5 proves it did not overflow. */
static char boundary_area[9];
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_ECHO_REFUSED ||     \
    YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_ECHO_REFUSED_BACKSPACE || \
    YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_ECHO_REFUSED_LINE_END
/* The same guarded layout as B5, because the refusal path is exactly where an
 * index computed from a half-updated counter would show up: the buffer occupies
 * indices 2-6 and the four sentinels must survive the call untouched. */
static char echo_area[9];

static void echo_area_reset(void)
{
    for (unsigned at = 0; at < sizeof echo_area; ++at) {
        echo_area[at] = (char)SENTINEL_BYTE;
    }
}

static void echo_area_check_sentinels(uint32_t before, uint32_t after)
{
    guest_check(echo_area[0] == (char)SENTINEL_BYTE &&
                    echo_area[1] == (char)SENTINEL_BYTE,
                before);
    guest_check(echo_area[7] == (char)SENTINEL_BYTE &&
                    echo_area[8] == (char)SENTINEL_BYTE,
                after);
}
#endif

/* B8. The order matters and is asserted: these calls are rejected before the
 * driver looks at the device, which is why they return INVALID_ARGUMENT in a
 * headless run instead of YAN_OS_UNAVAILABLE. */
static void check_invalid_arguments(void)
{
    length = UNWRITTEN;
    line[0] = (char)SENTINEL_BYTE;
    line[1] = (char)SENTINEL_BYTE;

    guest_check(yan_os_console_puts(NULL) == YAN_OS_INVALID_ARGUMENT,
                CHECK_B8_PUTS_NULL);
    guest_check(yan_os_console_getline(NULL, CONSOLE_LINE_CAPACITY, &length) ==
                    YAN_OS_INVALID_ARGUMENT,
                CHECK_B8_BUFFER_NULL);
    guest_check(yan_os_console_getline(line, 0, &length) ==
                    YAN_OS_INVALID_ARGUMENT,
                CHECK_B8_CAPACITY_ZERO);
    guest_check(yan_os_console_getline(line, CONSOLE_LINE_CAPACITY, NULL) ==
                    YAN_OS_INVALID_ARGUMENT,
                CHECK_B8_LENGTH_NULL);

    /* A rejected call reports the error and stores nothing at all. */
    guest_check(length == UNWRITTEN, CHECK_B8_LENGTH_UNTOUCHED);
    guest_check(line[0] == (char)SENTINEL_BYTE && line[1] == (char)SENTINEL_BYTE,
                CHECK_B8_BUFFER_UNTOUCHED);
}

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_HEADLESS
/* B1. No backend is attached, so CONNECTED reads 0 and all three calls must
 * return at once. The step limit the runner passes is the proof that none of
 * them waited: a poll for a ready bit that never comes would end the run with
 * "no termination" instead of a pass. */
static void run_scenario(void)
{
    length = UNWRITTEN;
    line[0] = (char)SENTINEL_BYTE;

    guest_check(yan_os_console_connected() == 0, CHECK_B1_NOT_CONNECTED);
    guest_check(yan_os_console_putc('x') == YAN_OS_UNAVAILABLE, CHECK_B1_PUTC);
    guest_check(yan_os_console_puts("x") == YAN_OS_UNAVAILABLE, CHECK_B1_PUTS);
    guest_check(yan_os_console_puts("") == YAN_OS_UNAVAILABLE,
                CHECK_B1_PUTS_EMPTY);
    guest_check(yan_os_console_getline(line, sizeof line, &length) ==
                    YAN_OS_UNAVAILABLE,
                CHECK_B1_GETLINE);
    guest_check(length == UNWRITTEN, CHECK_B1_LENGTH_UNTOUCHED);
    guest_check(line[0] == (char)SENTINEL_BYTE, CHECK_B1_BUFFER_UNTOUCHED);

    /* Repeating separates "returns immediately" from "returns once, then
     * blocks": a driver that consumed a one-shot condition would stop here. */
    for (unsigned round = 0; round < 8; ++round) {
        guest_check(yan_os_console_connected() == 0, CHECK_B1_REPEATED);
        guest_check(yan_os_console_putc('x') == YAN_OS_UNAVAILABLE,
                    CHECK_B1_REPEATED);
        guest_check(yan_os_console_puts("x") == YAN_OS_UNAVAILABLE,
                    CHECK_B1_REPEATED);
        guest_check(yan_os_console_getline(line, sizeof line, &length) ==
                        YAN_OS_UNAVAILABLE,
                    CHECK_B1_REPEATED);
    }
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_OUTPUT
/* B2. The runner must capture exactly "hi" and not one byte more. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_B2_CONNECTED);
    guest_check(yan_os_console_puts("hi") == YAN_OS_OK, CHECK_B2_PUTS_OK);
    /* An empty string writes nothing but is still a successful call. */
    guest_check(yan_os_console_puts("") == YAN_OS_OK, CHECK_B2_PUTS_EMPTY_OK);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_REFUSED
/* B3. The runner's backend accepts two bytes and then refuses: TX_READY reads 0
 * while CONNECTED still reads 1, which is the only state that separates "the
 * driver checked the ready bit" from "the driver only checked the connection".
 * The capture must be exactly "hi": a refused byte is reported, not delivered,
 * and not retried. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_B3_CONNECTED);
    guest_check(yan_os_console_puts("hi") == YAN_OS_OK, CHECK_B3_PUTS_OK);
    guest_check(yan_os_console_putc('X') == YAN_OS_UNAVAILABLE,
                CHECK_B3_PUTC_REFUSED);
    guest_check(yan_os_console_puts("YZ") == YAN_OS_UNAVAILABLE,
                CHECK_B3_PUTS_STOPS);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_EDIT
/* B4. Input is two backspaces on an empty line, then "ab", one backspace and
 * "c", then LF. The backspaces on the empty line must leave no trace at all,
 * so the captured echo must be exactly "ab\b \bc\r\n". */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_B4_CONNECTED);

    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(line, sizeof line, &length);
    guest_check(result == YAN_OS_OK, CHECK_B4_RESULT);
    guest_check(length == 2, CHECK_B4_LENGTH);
    guest_check(line[0] == 'a' && line[1] == 'c' && line[2] == '\0',
                CHECK_B4_TEXT);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_BOUNDARY
/* B5. The runner sends "abcde\n" into a capacity-5 buffer, so only four
 * characters fit. The fifth is ignored - not stored, not echoed - and the line
 * still ends at the LF with length == capacity - 1. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_B5_CONNECTED);

    for (unsigned at = 0; at < sizeof boundary_area; ++at) {
        boundary_area[at] = (char)SENTINEL_BYTE;
    }
    boundary_area[8] = (char)0x5a;
    char *buffer = &boundary_area[2];

    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(buffer, 5, &length);
    guest_check(result == YAN_OS_OK, CHECK_B5_RESULT);
    guest_check(length == 4, CHECK_B5_LENGTH);
    guest_check(buffer[0] == 'a' && buffer[1] == 'b' && buffer[2] == 'c' &&
                    buffer[3] == 'd',
                CHECK_B5_TEXT);
    guest_check(buffer[4] == '\0', CHECK_B5_TERMINATOR);
    guest_check(boundary_area[0] == (char)SENTINEL_BYTE &&
                    boundary_area[1] == (char)SENTINEL_BYTE,
                CHECK_B5_SENTINEL_BEFORE);
    guest_check(boundary_area[7] == (char)SENTINEL_BYTE &&
                    boundary_area[8] == (char)0x5a,
                CHECK_B5_SENTINEL_AFTER);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_CRLF
/* B6. The runner sends "a\r\nb\n": two lines, because the LF that follows the CR
 * belongs to the line the CR ended. The echo must be exactly "a\r\nb\r\n". */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_B6_CONNECTED);

    length = UNWRITTEN;
    YanOsResult result = yan_os_console_getline(line, sizeof line, &length);
    guest_check(result == YAN_OS_OK, CHECK_B6_FIRST_RESULT);
    guest_check(length == 1, CHECK_B6_FIRST_LENGTH);
    guest_check(line[0] == 'a' && line[1] == '\0', CHECK_B6_FIRST_TEXT);

    length = UNWRITTEN;
    result = yan_os_console_getline(line, sizeof line, &length);
    guest_check(result == YAN_OS_OK, CHECK_B6_SECOND_RESULT);
    guest_check(length == 1, CHECK_B6_SECOND_LENGTH);
    guest_check(line[0] == 'b' && line[1] == '\0', CHECK_B6_SECOND_TEXT);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_DISCONNECT
/* B7. The runner sends "ab" with no line ending and then detaches the terminal
 * while this call is waiting. The driver must notice at its next iteration and
 * return YAN_OS_UNAVAILABLE instead of waiting for a byte that can never
 * arrive; the runner's step limit turns a driver that keeps polling into a
 * reported "no termination". */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_B7_CONNECTED);

    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(line, sizeof line, &length);
    guest_check(result == YAN_OS_UNAVAILABLE, CHECK_B7_RESULT);
    guest_check(length == UNWRITTEN, CHECK_B7_LENGTH_UNTOUCHED);
    /* The Guest confirms the cause itself: the terminal really is gone, so the
     * early return was not a driver that gave up while a backend was attached. */
    guest_check(yan_os_console_connected() == 0, CHECK_B7_DISCONNECTED);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_HIGH_BYTE
/* A byte above 0x7f is not a control character; the spec's out-of-scope note
 * ("non-ASCII bytes are passed through as printable characters") decides it is
 * stored and echoed unchanged. Input is 0x80 followed by LF. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_HIGH_CONNECTED);

    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(line, sizeof line, &length);
    guest_check(result == YAN_OS_OK, CHECK_HIGH_RESULT);
    guest_check(length == 1, CHECK_HIGH_LENGTH);
    guest_check((unsigned char)line[0] == 0x80, CHECK_HIGH_BYTE);
    guest_check(line[1] == '\0', CHECK_HIGH_TERMINATOR);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_ECHO_REFUSED
/* The echo of the first printable character is refused, so nothing can be
 * delivered and the capture must be empty. The character itself is stored before
 * the echo is attempted, which is why the buffer reads "a" and stays terminated;
 * the failure is reported through the result, not through the buffer. The feed
 * carries a line ending after the failure point so that a driver which ignores
 * the refusal completes the line and fails the result check here instead of
 * waiting for input forever. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_ECHO_REFUSED_CONNECTED);

    echo_area_reset();
    char *buffer = &echo_area[2];
    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(buffer, 5, &length);
    guest_check(result == YAN_OS_UNAVAILABLE, CHECK_ECHO_REFUSED_RESULT);
    guest_check(length == UNWRITTEN, CHECK_ECHO_REFUSED_LENGTH_UNTOUCHED);
    guest_check(buffer[0] == 'a' && buffer[1] == '\0', CHECK_ECHO_REFUSED_TEXT);
    echo_area_check_sentinels(CHECK_ECHO_REFUSED_SENTINEL_BEFORE,
                              CHECK_ECHO_REFUSED_SENTINEL_AFTER);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_ECHO_REFUSED_BACKSPACE
/* The backspace echo "\b \b" is refused: "ab" was accepted and echoed, then the
 * backspace that erases the "b" cannot be echoed. The erased character must
 * still be gone from the buffer and the buffer must still be terminated, while
 * the caller hears YAN_OS_UNAVAILABLE. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_ECHO_BACKSPACE_CONNECTED);

    echo_area_reset();
    char *buffer = &echo_area[2];
    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(buffer, 5, &length);
    guest_check(result == YAN_OS_UNAVAILABLE, CHECK_ECHO_BACKSPACE_RESULT);
    guest_check(length == UNWRITTEN, CHECK_ECHO_BACKSPACE_LENGTH_UNTOUCHED);
    guest_check(buffer[0] == 'a' && buffer[1] == '\0',
                CHECK_ECHO_BACKSPACE_TEXT);
    echo_area_check_sentinels(CHECK_ECHO_BACKSPACE_SENTINEL_BEFORE,
                              CHECK_ECHO_BACKSPACE_SENTINEL_AFTER);
}
#endif

#if YAN_CONSOLE_SCENARIO == YAN_CONSOLE_SCENARIO_ECHO_REFUSED_LINE_END
/* The line ending itself cannot be echoed: 'a' was accepted and echoed, then the
 * CR ends the line and its "\r\n" is refused. The line is complete on the wire
 * but not on the screen, so the caller must not be told the line was read: the
 * result is YAN_OS_UNAVAILABLE and *length stays unwritten. */
static void run_scenario(void)
{
    guest_check(yan_os_console_connected() != 0, CHECK_ECHO_LINE_END_CONNECTED);

    echo_area_reset();
    char *buffer = &echo_area[2];
    length = UNWRITTEN;
    const YanOsResult result = yan_os_console_getline(buffer, 5, &length);
    guest_check(result == YAN_OS_UNAVAILABLE, CHECK_ECHO_LINE_END_RESULT);
    guest_check(length == UNWRITTEN, CHECK_ECHO_LINE_END_LENGTH_UNTOUCHED);
    guest_check(buffer[0] == 'a' && buffer[1] == '\0', CHECK_ECHO_LINE_END_TEXT);
    echo_area_check_sentinels(CHECK_ECHO_LINE_END_SENTINEL_BEFORE,
                              CHECK_ECHO_LINE_END_SENTINEL_AFTER);
}
#endif

void main(void)
{
    check_invalid_arguments();
    run_scenario();
    /* tohost == 1 is the pass code shared with every Guest image in this tree. */
    guest_finish(1);
}