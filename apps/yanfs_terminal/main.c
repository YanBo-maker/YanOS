/* Production Guest terminal for 0022.
 *
 * This is the application half of docs/specs/0022-terminal-file-operations.md:
 * it wires the real platform devices to the shell (os/shell.c), the interrupt
 * line reader (os/line.c) and the UART terminal (os/terminal.c), and it owns
 * the whole storage stack (os/yanfs.c over os/yanfs_block.c). It references
 * nothing under tests/, so the production image and the validation corpus stay
 * separate.
 *
 * Layout:
 *   main          runs on the boot stack from os/trap_entry.S, configures the
 *                 transport and UART PLIC routes, checks that the host channel
 *                 and its block geometry are usable, spawns the one application
 *                 task and enters the scheduler. It never performs a blocking
 *                 filesystem or block call: those run in the task, where the
 *                 4 KiB stack is available. mie.MEIE is the scheduler's.
 *   app_task      opens the terminal (which requires CONNECTED), initializes
 *                 the block adapter and the filesystem, mounts, initializes the
 *                 shell and runs the read/dispatch loop. The filesystem, the
 *                 adapter, the shell and the terminal are static because they
 *                 are far larger than a task stack.
 *
 * Verdicts: tohost 1 is the healthy exit after a successful unmount and
 * terminal close. Every other end writes 0x71000000 | reason, so a host can
 * tell an application failure from the runtime's panic codes (0x80000000) and
 * the filesystem corpus' 0x60000000. The exact output lines are listed in the
 * delivery report; a failure prints "ERROR <token>\r\n" and the mount failure
 * prints the filesystem result's own name.
 *
 * Output goes through yan_terminal_putc, which re-checks the connection and TX
 * readiness after every byte (including the last), so a host write failure is
 * never silently ignored. A write command that has already committed its
 * metadata cannot be rolled back; the application claims nothing to the
 * contrary. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "line.h"
#include "platform.h"
#include "shell.h"
#include "task.h"
#include "terminal.h"
#include "yanfs.h"
#include "yanfs_block.h"

/* The finishing word lives in os/trap_entry.S' .tohost section. */
extern volatile uint32_t tohost;

#define APP_FAIL_TOP UINT32_C(0x71000000)
#define APP_FAIL(reason) (APP_FAIL_TOP | (reason))

#define APP_REASON_NO_CHANNEL 1u
#define APP_REASON_MAX_COUNT 2u
#define APP_REASON_SPAWN 3u
#define APP_REASON_ADAPTER 4u
#define APP_REASON_FS_INIT 5u
#define APP_REASON_MOUNT 6u
#define APP_REASON_NO_TERMINAL 7u
#define APP_REASON_SHELL_INIT 8u
#define APP_REASON_LINE_UNAVAILABLE 9u
#define APP_REASON_LINE_INVALID 10u
#define APP_REASON_LINE_BUSY 11u
#define APP_REASON_SHELL_FATAL 12u
#define APP_REASON_SHELL_INVALID 13u
#define APP_REASON_SHELL_BUSY 14u
#define APP_REASON_UNMOUNT 15u
#define APP_REASON_CLOSE 16u
#define APP_REASON_OUTPUT 17u

/* ---------------------------------------------------------- static storage */

static YanFs fs;
static YanFsBlockAdapter adapter;
static YanShell shell;
static YanTerminal terminal;

/* --------------------------------------------------------------- reporting */

/* Writes tohost and stops. The value is the machine-visible verdict; the loop
 * is where the Guest waits for the host to end the run, exactly as the runtime
 * panic path does. */
_Noreturn static void app_finish(uint32_t code)
{
    tohost = code;
    for (;;) {
    }
}

/* Every produced byte goes through the application's one checked sink.
 * yan_terminal_putc remembers a refusal session-wide - including a line echo,
 * which goes through the terminal without passing here - so checking
 * yan_terminal_output_failed() first keeps the shell sink, the application's
 * messages and the echo closed together: after the first false, no path calls
 * the terminal again. */
static bool app_putc(void *context, uint8_t byte)
{
    (void)context;
    if (yan_terminal_output_failed()) {
        return false;
    }
    return yan_terminal_putc(byte);
}

static bool app_write(const char *text)
{
    if (yan_terminal_output_failed()) {
        return false;
    }
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (!app_putc(NULL, text[i])) {
            return false;
        }
    }
    return true;
}

static const char *fs_result_name(YanFsResult result)
{
    switch (result) {
    case YAN_FS_OK: return "OK";
    case YAN_FS_END: return "END";
    case YAN_FS_INVALID: return "INVALID";
    case YAN_FS_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_FS_FAULTED: return "FAULTED";
    case YAN_FS_BUSY: return "BUSY";
    case YAN_FS_EXISTS: return "EXISTS";
    case YAN_FS_NOT_FOUND: return "NOT_FOUND";
    case YAN_FS_DIRECTORY_FULL: return "DIRECTORY_FULL";
    case YAN_FS_NOSPACE: return "NOSPACE";
    case YAN_FS_CORRUPT: return "CORRUPT";
    case YAN_FS_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_FS_IO: return "IO";
    case YAN_FS_PROTOCOL: return "PROTOCOL";
    }
    return "UNKNOWN";
}

/* Best effort at a diagnostic only while the sink is healthy, and
 * short-circuiting inside it: the first refused byte closes output for good,
 * so the later segments are not attempted. Then the failure code. */
_Noreturn static void app_fail(uint32_t reason, const char *token)
{
    if (!yan_terminal_output_failed()) {
        if (app_write("ERROR ")) {
            if (app_write(token)) {
                (void)app_write("\r\n");
            }
        }
    }
    app_finish(APP_FAIL(reason));
}

/* A mount failure names the filesystem's own result, which is the stable token
 * a Guest oracle can match. Same short-circuit as app_fail. */
_Noreturn static void app_mount_fail(YanFsResult result)
{
    if (!yan_terminal_output_failed()) {
        if (app_write("ERROR ")) {
            if (app_write(fs_result_name(result))) {
                (void)app_write("\r\n");
            }
        }
    }
    app_finish(APP_FAIL(APP_REASON_MOUNT));
}

/* ------------------------------------------------------------- the routes */

/* Below the CPU: the caller's job per 0019. Both device interrupt enables and
 * both PLIC sources are configured here, before the task exists. The UART
 * receive enable is re-masked and re-armed by the line reader around each
 * byte; arming it now only means an early byte latches with its interrupt
 * pending, which the reader's ready check will see. */
static void app_configure_route(void)
{
    yan_os_transport_set_irq_enable(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_uart_set_rx_irq(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_UART, 1);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_UART, 1);
    yan_os_plic_set_threshold(0);
}

/* ------------------------------------------------------------ the session */

_Noreturn static void app_session(void)
{
    /* The terminal must own the receive path before any byte is read; without
     * a connected backend there is nothing to report and nothing to wait for,
     * so this ends immediately. */
    if (yan_terminal_open(&terminal) != YAN_LINE_OK) {
        app_finish(APP_FAIL(APP_REASON_NO_TERMINAL));
    }

    const YanFsBlockIo io = yan_fs_block_backend(&adapter);
    if (yan_fs_block_init(&adapter) != YAN_FS_OK) {
        app_fail(APP_REASON_ADAPTER, "ADAPTER");
    }
    if (yan_fs_init(&fs, io) != YAN_FS_OK) {
        app_fail(APP_REASON_FS_INIT, "FS_INIT");
    }
    const YanFsResult mounted = yan_fs_mount(&fs);
    if (mounted != YAN_FS_OK) {
        app_mount_fail(mounted);
    }
    YanShellOutput output;
    output.context = NULL;
    output.putc = app_putc;
    if (yan_shell_init(&shell, &fs, output) != YAN_SHELL_OK) {
        app_fail(APP_REASON_SHELL_INIT, "SHELL_INIT");
    }
    if (!app_write("yanfs terminal\r\n")) {
        app_fail(APP_REASON_OUTPUT, "OUTPUT");
    }

    for (;;) {
        if (!app_write("yanfs> ")) {
            app_fail(APP_REASON_OUTPUT, "OUTPUT");
        }
        const YanLineResult line = yan_terminal_next(&terminal);
        if (line == YAN_LINE_OK) {
            if (terminal.line.length == 0u) {
                /* A bare Enter only asks for the next prompt. */
                continue;
            }
            const YanShellResult result =
                yan_shell_execute(&shell, terminal.line.buffer,
                                  terminal.line.length);
            if (result == YAN_SHELL_OK) {
                continue;
            }
            if (result == YAN_SHELL_EXIT) {
                /* A healthy exit is only healthy if the storage state can be
                 * given up cleanly: unmount first, then release the terminal
                 * and report success. */
                if (yan_fs_unmount(&fs) != YAN_FS_OK) {
                    app_fail(APP_REASON_UNMOUNT, "UNMOUNT");
                }
                if (yan_terminal_close(&terminal) != YAN_LINE_OK) {
                    app_fail(APP_REASON_CLOSE, "CLOSE");
                }
                /* The success tail is not exempt: a refused byte here means
                 * the host never saw the whole line, so this is not a healthy
                 * exit and tohost must not be 1. */
                if (!app_write("yanfs: exit\r\n")) {
                    app_finish(APP_FAIL(APP_REASON_OUTPUT));
                }
                app_finish(1u);
            }
            if (result == YAN_SHELL_FATAL) {
                /* The shell has already printed the filesystem's ERROR line.
                 * Release the receive line without stealing a busy read and
                 * stop: no next command, no retry, no remount. */
                (void)yan_terminal_close(&terminal);
                app_fail(APP_REASON_SHELL_FATAL, "SHELL_FATAL");
            }
            /* INVALID or BUSY here is an unexpected API result. */
            (void)yan_terminal_close(&terminal);
            app_fail(result == YAN_SHELL_BUSY ? APP_REASON_SHELL_BUSY
                                              : APP_REASON_SHELL_INVALID,
                     result == YAN_SHELL_BUSY ? "SHELL_BUSY" : "SHELL_INVALID");
        }
        if (line == YAN_LINE_TOO_LONG) {
            if (!app_write("ERROR LINE_TOO_LONG\r\n")) {
                app_fail(APP_REASON_OUTPUT, "OUTPUT");
            }
            continue;
        }
        if (line == YAN_LINE_INVALID_INPUT) {
            if (!app_write("ERROR INVALID_INPUT\r\n")) {
                app_fail(APP_REASON_OUTPUT, "OUTPUT");
            }
            continue;
        }
        /* UNAVAILABLE, INVALID and BUSY are anomalies that end the session. */
        (void)yan_terminal_close(&terminal);
        if (line == YAN_LINE_UNAVAILABLE) {
            app_fail(APP_REASON_LINE_UNAVAILABLE, "LINE_UNAVAILABLE");
        }
        if (line == YAN_LINE_BUSY) {
            app_fail(APP_REASON_LINE_BUSY, "LINE_BUSY");
        }
        app_fail(APP_REASON_LINE_INVALID, "LINE_INVALID");
    }
}

static void app_task(void *argument)
{
    (void)argument;
    app_session();
}

int main(void)
{
    /* main runs before any task exists, so it must not block. The channel and
     * its block geometry are readable state, not requests. A startup failure
     * still prints its stable token when the UART is connected. */
    if (!yan_os_transport_host_ready()) {
        app_fail(APP_REASON_NO_CHANNEL, "NO_CHANNEL");
    }
    if (yan_os_block_max_count() != 1u) {
        app_fail(APP_REASON_MAX_COUNT, "MAX_COUNT");
    }
    app_configure_route();
    if (yan_os_task_spawn(app_task, NULL) != YAN_OS_TASK_OK) {
        app_fail(APP_REASON_SPAWN, "SPAWN");
    }
    yan_os_sched_run();
    /* The scheduler never returns. */
    app_fail(APP_REASON_SPAWN, "SPAWN");
}
