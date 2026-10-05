/* YanFS end-to-end Guest: one filesystem task over the real block adapter.
 *
 * One source, three roles selected by -DYANFS_ROLE:
 *
 *   writer (1)  runs the whole document sequence on a freshly formatted image
 *               and exits through tohost once every result has been checked;
 *   reader (2)  is a *fresh process* on the image the writer left behind and
 *               reads/lists/stats the terminal state without writing a byte;
 *   corrupt (3) mounts an intentionally damaged image and requires the exact
 *               error class the host built (CORRUPT or UNSUPPORTED), with the
 *               instance left UNMOUNTED and nothing written.
 *
 * The application uses only yan_fs_* and the adapter's public backend; it never
 * touches the directory or the block layer by hand. The filesystem context, the
 * adapter, the 5000-byte input and the read-back buffer are all static: the
 * cooperative runtime gives a task a 4 KiB stack and YanFs alone is larger.
 *
 * Verdicts: tohost 1 means the role passed. A failure prints
 *   :FAIL: yanfs code=0x60000000|reason phase=N
 * and writes the same code to tohost, so a host can require the marker and the
 * executor's failure code to agree. The 0x60000000 prefix keeps these codes
 * apart from the runtime panic codes (0x80000000) and the other cases' 0x4... /
 * 0x5... prefixes. The host harness is tests/guest/run_yanfs.py. */
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "console.h"
#include "guest.h"
#include "platform.h"
#include "task.h"
#include "yanfs.h"
#include "yanfs_block.h"

#define YC_ROLE_WRITER 1
#define YC_ROLE_READER 2
#define YC_ROLE_CORRUPT 3

#ifndef YANFS_ROLE
#define YANFS_ROLE YC_ROLE_WRITER
#endif
#ifndef YANFS_CORRUPT_EXPECT
#define YANFS_CORRUPT_EXPECT 10 /* YAN_FS_CORRUPT */
#endif

/* ------------------------------------------------------------ fail reasons */

#define YC_FAIL(reason) (UINT32_C(0x60000000) | (reason))

#define YC_NO_CHANNEL 1u
#define YC_MAX_COUNT 2u
#define YC_SPAWN 3u
#define YC_ADAPTER_INIT 4u
#define YC_FS_INIT 5u
#define YC_MOUNT 6u
#define YC_CREATE_HELLO 7u
#define YC_READ_HELLO 8u
#define YC_STAT_HELLO 9u
#define YC_LIST_HELLO 10u
#define YC_CREATE_GUARD 11u
#define YC_REPLACE_HELLO 12u
#define YC_READ_HELLO_LONG 13u
#define YC_READ_HELLO_EOF 14u
#define YC_REMOVE_HELLO 15u
#define YC_STAT_REMOVED 16u
#define YC_CREATE_PERSIST 17u
#define YC_CREATE_EMPTY 18u
#define YC_CREATE_REUSE 19u
#define YC_DUP_EXISTS 20u
#define YC_REPLACE_ABSENT 21u
#define YC_LIST_ORDER 22u
#define YC_STAT_SIZES 23u
#define YC_BADPTR_CREATE 24u
#define YC_BADPTR_READ 25u
#define YC_UNMOUNT 26u
#define YC_READ_PERSIST 30u
#define YC_READ_GUARD 31u
#define YC_READ_EMPTY 32u
#define YC_READ_REUSE 33u
#define YC_READER_LIST 34u
#define YC_CORRUPT_RESULT 40u
#define YC_CORRUPT_STATE 41u

/* ------------------------------------------------------------- the console */

static volatile uint32_t yc_phase = 0;

static void yc_puts(const char *text)
{
    (void)yan_os_console_puts(text);
}

static void yc_put_hex(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    (void)yan_os_console_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        (void)yan_os_console_putc(digits[(value >> (unsigned)shift) & 0xfu]);
    }
}

static void yc_put_dec(uint32_t value)
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

__attribute__((noreturn)) static void yc_fail(uint32_t reason)
{
    yc_puts(":FAIL: yanfs code=");
    yc_put_hex(YC_FAIL(reason));
    yc_puts(" phase=");
    yc_put_dec(yc_phase);
    yc_puts("\r\n");
    guest_finish(YC_FAIL(reason));
    for (;;) {
    }
}

/* ---------------------------------------------------------- static storage */

static YanFs fs;
static YanFsBlockAdapter adapter;
#if YANFS_ROLE == YC_ROLE_WRITER
static uint8_t input[5000];
#endif
#if YANFS_ROLE != YC_ROLE_CORRUPT
static uint8_t readback[5000];
#endif

/* ------------------------------------------------------------- byte patterns
 *
 * Every pattern is a function of the byte's index inside its file, reproduced
 * independently by run_yanfs.py. Helpers used by only one role are compiled only
 * for that role so the strict build has no unused static function. */

static void yc_print_result(const char *what, uint32_t length, YanFsResult result)
{
    yc_puts("yanfs: ");
    yc_puts(what);
    yc_puts(" len=");
    yc_put_dec(length);
    yc_puts(" result=");
    yc_put_dec((uint32_t)result);
    yc_puts("\r\n");
}

#if YANFS_ROLE == YC_ROLE_WRITER

static uint8_t yc_hello_byte(uint32_t i)
{
    return (uint8_t)((i * 17u + (i >> 8) * 29u + 41u) & 255u);
}

static void yc_fill(uint8_t *buffer, uint32_t length, uint8_t (*byte_at)(uint32_t))
{
    for (uint32_t i = 0; i < length; ++i) {
        buffer[i] = byte_at(i);
    }
}

#endif /* writer */

#if YANFS_ROLE != YC_ROLE_CORRUPT

static uint8_t yc_guard_byte(uint32_t i)
{
    return (uint8_t)((i * 7u + 9u) & 255u);
}

static uint8_t yc_persist_byte(uint32_t i)
{
    return (uint8_t)((i * 13u + (i >> 8) * 5u + 71u) & 255u);
}

static uint8_t yc_reuse_byte(uint32_t i)
{
    return (uint8_t)((i * 3u + 101u) & 255u);
}

static void yc_expect_bytes(const uint8_t *buffer, uint32_t length,
                            uint8_t (*byte_at)(uint32_t), uint32_t reason)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (buffer[i] != byte_at(i)) {
            yc_fail(reason);
        }
    }
}

static void yc_require_ok(const char *what, uint32_t length, YanFsResult result,
                          uint32_t reason)
{
    yc_print_result(what, length, result);
    if (result != YAN_FS_OK) {
        yc_fail(reason);
    }
}

#if YANFS_ROLE == YC_ROLE_WRITER
static void yc_require_result(const char *what, uint32_t length, YanFsResult result,
                              YanFsResult expected, uint32_t reason)
{
    yc_print_result(what, length, result);
    if (result != expected) {
        yc_fail(reason);
    }
}
#endif

static void yc_check_list(const char **names, const uint32_t *sizes,
                          uint32_t reason)
{
    uint32_t cursor = 0;
    YanFsInfo info;
    for (uint32_t i = 0; i < 4u; ++i) {
        if (yan_fs_list(&fs, &cursor, &info) != YAN_FS_OK) {
            yc_fail(reason);
        }
        uint32_t wanted = 0;
        while (names[i][wanted] != '\0') {
            ++wanted;
        }
        if (wanted != (uint32_t)strlen(info.name) ||
            memcmp(info.name, names[i], wanted) != 0) {
            yc_fail(reason);
        }
        if (info.size_bytes != sizes[i]) {
            yc_fail(reason);
        }
        yc_puts("yanfs: list[");
        yc_put_dec(i);
        yc_puts("]=");
        yc_puts(info.name);
        yc_puts(" size=");
        yc_put_dec(info.size_bytes);
        yc_puts("\r\n");
    }
    if (yan_fs_list(&fs, &cursor, &info) != YAN_FS_END) {
        yc_fail(reason);
    }
}

#endif /* != corrupt */

/* --------------------------------------------------------------- writer role */

#if YANFS_ROLE == YC_ROLE_WRITER

static void yc_writer(void)
{
    YanFsBlockIo io = yan_fs_block_backend(&adapter);
    YanFsInfo info;

    yc_phase = 1;
    if (yan_fs_block_init(&adapter) != YAN_FS_OK) {
        yc_fail(YC_ADAPTER_INIT);
    }
    if (yan_fs_init(&fs, io) != YAN_FS_OK) {
        yc_fail(YC_FS_INIT);
    }

    yc_phase = 2;
    if (yan_fs_mount(&fs) != YAN_FS_OK) {
        yc_fail(YC_MOUNT);
    }

    yc_phase = 3;
    yc_require_ok("create hello.txt", 5u,
                  yan_fs_create(&fs, "hello.txt", (const uint8_t *)"hello", 5u),
                  YC_CREATE_HELLO);
    uint32_t got = 0u;
    yc_require_ok("read hello.txt", 5u,
                  yan_fs_read(&fs, "hello.txt", 0u, readback, 5u, &got),
                  YC_READ_HELLO);
    if (got != 5u || memcmp(readback, "hello", 5u) != 0) {
        yc_fail(YC_READ_HELLO);
    }
    yc_require_ok("stat hello.txt", 5u, yan_fs_stat(&fs, "hello.txt", &info),
                  YC_STAT_HELLO);
    if (info.size_bytes != 5u) {
        yc_fail(YC_STAT_HELLO);
    }
    {
        uint32_t cursor = 0;
        if (yan_fs_list(&fs, &cursor, &info) != YAN_FS_OK ||
            strlen(info.name) != 9u || memcmp(info.name, "hello.txt", 9u) != 0 ||
            info.size_bytes != 5u) {
            yc_fail(YC_LIST_HELLO);
        }
        if (yan_fs_list(&fs, &cursor, &info) != YAN_FS_END) {
            yc_fail(YC_LIST_HELLO);
        }
    }

    yc_phase = 4;
    yc_fill(input, 17u, yc_guard_byte);
    yc_require_ok("create guard.bin", 17u, yan_fs_create(&fs, "guard.bin", input, 17u),
                  YC_CREATE_GUARD);

    yc_phase = 5;
    yc_fill(input, 5000u, yc_hello_byte);
    yc_require_ok("replace hello.txt", 5000u,
                  yan_fs_replace(&fs, "hello.txt", input, 5000u), YC_REPLACE_HELLO);

    yc_phase = 6;
    yc_require_ok("read hello.txt 5000", 5000u,
                  yan_fs_read(&fs, "hello.txt", 0u, readback, 5000u, &got),
                  YC_READ_HELLO_LONG);
    if (got != 5000u) {
        yc_fail(YC_READ_HELLO_LONG);
    }
    yc_expect_bytes(readback, 5000u, yc_hello_byte, YC_READ_HELLO_LONG);
    yc_require_ok("read hello.txt 4090..4110", 20u,
                  yan_fs_read(&fs, "hello.txt", 4090u, readback, 20u, &got),
                  YC_READ_HELLO_LONG);
    if (got != 20u) {
        yc_fail(YC_READ_HELLO_LONG);
    }
    for (uint32_t i = 0; i < 20u; ++i) {
        if (readback[i] != yc_hello_byte(4090u + i)) {
            yc_fail(YC_READ_HELLO_LONG);
        }
    }
    yc_require_ok("read hello.txt at EOF", 10u,
                  yan_fs_read(&fs, "hello.txt", 5000u, readback, 10u, &got),
                  YC_READ_HELLO_EOF);
    if (got != 0u) {
        yc_fail(YC_READ_HELLO_EOF);
    }

    yc_phase = 7;
    yc_require_ok("remove hello.txt", 0u, yan_fs_remove(&fs, "hello.txt"),
                  YC_REMOVE_HELLO);
    yc_require_result("stat removed hello.txt", 0u,
                      yan_fs_stat(&fs, "hello.txt", &info), YAN_FS_NOT_FOUND,
                      YC_STAT_REMOVED);

    yc_phase = 8;
    yc_fill(input, 5000u, yc_persist_byte);
    yc_require_ok("create persist.bin", 5000u,
                  yan_fs_create(&fs, "persist.bin", input, 5000u), YC_CREATE_PERSIST);
    yc_require_ok("create empty.txt", 0u, yan_fs_create(&fs, "empty.txt", NULL, 0u),
                  YC_CREATE_EMPTY);
    yc_fill(input, 5u, yc_reuse_byte);
    yc_require_ok("create reuse.txt", 5u,
                  yan_fs_create(&fs, "reuse.txt", input, 5u), YC_CREATE_REUSE);

    yc_phase = 9;
    yc_require_result("duplicate guard.bin", 17u,
                      yan_fs_create(&fs, "guard.bin", input, 17u), YAN_FS_EXISTS,
                      YC_DUP_EXISTS);
    yc_require_result("replace absent.bin", 1u,
                      yan_fs_replace(&fs, "absent.bin", input, 1u), YAN_FS_NOT_FOUND,
                      YC_REPLACE_ABSENT);

    yc_phase = 10;
    {
        const char *names[4] = {"persist.bin", "guard.bin", "empty.txt", "reuse.txt"};
        const uint32_t sizes[4] = {5000u, 17u, 0u, 5u};
        yc_check_list(names, sizes, YC_LIST_ORDER);
    }
    yc_require_ok("stat persist.bin", 5000u,
                  yan_fs_stat(&fs, "persist.bin", &info), YC_STAT_SIZES);
    if (info.size_bytes != 5000u) {
        yc_fail(YC_STAT_SIZES);
    }

    /* The real 32-bit pointer-domain negative controls. These addresses are
     * only declared ranges: the core must reject them before reading or writing
     * through them, so a Guest that widened the check to uint64_t and only
     * looked for a wrap would let them through. */
    yc_phase = 11;
    yc_require_result(
        "create badptr", 16u,
        yan_fs_create(&fs, "badptr", (const uint8_t *)(uintptr_t)(UINT32_MAX - 7u),
                      16u),
        YAN_FS_INVALID, YC_BADPTR_CREATE);
    yc_require_result("stat badptr", 0u, yan_fs_stat(&fs, "badptr", &info),
                      YAN_FS_NOT_FOUND, YC_BADPTR_CREATE);
    got = 123u;
    yc_require_result(
        "read badptr out", 16u,
        yan_fs_read(&fs, "persist.bin", 0u,
                    (uint8_t *)(uintptr_t)(UINT32_MAX - 7u), 16u, &got),
        YAN_FS_INVALID, YC_BADPTR_READ);
    if (got != 0u) {
        yc_fail(YC_BADPTR_READ);
    }

    yc_phase = 12;
    yc_require_ok("unmount", 0u, yan_fs_unmount(&fs), YC_UNMOUNT);

    yc_puts("yanfs writer PASS\r\n");
    guest_finish(1u);
    for (;;) {
    }
}

#endif /* writer */

/* --------------------------------------------------------------- reader role */

#if YANFS_ROLE == YC_ROLE_READER

static void yc_reader(void)
{
    YanFsBlockIo io = yan_fs_block_backend(&adapter);
    YanFsInfo info;

    yc_phase = 1;
    if (yan_fs_block_init(&adapter) != YAN_FS_OK) {
        yc_fail(YC_ADAPTER_INIT);
    }
    if (yan_fs_init(&fs, io) != YAN_FS_OK) {
        yc_fail(YC_FS_INIT);
    }
    yc_phase = 2;
    if (yan_fs_mount(&fs) != YAN_FS_OK) {
        yc_fail(YC_MOUNT);
    }
    uint32_t got = 0u;
    yc_require_ok("reader read persist.bin", 5000u,
                  yan_fs_read(&fs, "persist.bin", 0u, readback, 5000u, &got),
                  YC_READ_PERSIST);
    if (got != 5000u) {
        yc_fail(YC_READ_PERSIST);
    }
    yc_expect_bytes(readback, 5000u, yc_persist_byte, YC_READ_PERSIST);

    yc_phase = 3;
    yc_require_ok("reader read guard.bin", 17u,
                  yan_fs_read(&fs, "guard.bin", 0u, readback, 17u, &got),
                  YC_READ_GUARD);
    if (got != 17u) {
        yc_fail(YC_READ_GUARD);
    }
    yc_expect_bytes(readback, 17u, yc_guard_byte, YC_READ_GUARD);
    yc_require_ok("reader read empty.txt", 0u,
                  yan_fs_read(&fs, "empty.txt", 0u, NULL, 0u, &got), YC_READ_EMPTY);
    if (got != 0u) {
        yc_fail(YC_READ_EMPTY);
    }
    yc_require_ok("reader read reuse.txt", 5u,
                  yan_fs_read(&fs, "reuse.txt", 0u, readback, 5u, &got),
                  YC_READ_REUSE);
    if (got != 5u) {
        yc_fail(YC_READ_REUSE);
    }
    yc_expect_bytes(readback, 5u, yc_reuse_byte, YC_READ_REUSE);

    yc_phase = 4;
    {
        const char *names[4] = {"persist.bin", "guard.bin", "empty.txt", "reuse.txt"};
        const uint32_t sizes[4] = {5000u, 17u, 0u, 5u};
        yc_check_list(names, sizes, YC_READER_LIST);
    }
    yc_require_ok("reader stat persist.bin", 5000u,
                  yan_fs_stat(&fs, "persist.bin", &info), YC_READER_LIST);
    if (info.size_bytes != 5000u) {
        yc_fail(YC_READER_LIST);
    }

    yc_puts("yanfs reader PASS\r\n");
    guest_finish(1u);
    for (;;) {
    }
}

#endif /* reader */

/* -------------------------------------------------------------- corrupt role */

#if YANFS_ROLE == YC_ROLE_CORRUPT

static void yc_corrupt(void)
{
    YanFsBlockIo io = yan_fs_block_backend(&adapter);

    yc_phase = 1;
    if (yan_fs_block_init(&adapter) != YAN_FS_OK) {
        yc_fail(YC_ADAPTER_INIT);
    }
    if (yan_fs_init(&fs, io) != YAN_FS_OK) {
        yc_fail(YC_FS_INIT);
    }
    yc_phase = 2;
    YanFsResult result = yan_fs_mount(&fs);
    yc_print_result("mount damaged image", 0u, result);
    if (result != (YanFsResult)YANFS_CORRUPT_EXPECT) {
        yc_fail(YC_CORRUPT_RESULT);
    }
    if (fs.state != YAN_FS_UNMOUNTED) {
        yc_fail(YC_CORRUPT_STATE);
    }
    yc_puts("yanfs corrupt-rejected PASS\r\n");
    guest_finish(1u);
    for (;;) {
    }
}

#endif /* corrupt */

/* ==================================================================== main */

static void yc_fs_task(void *argument)
{
    (void)argument;
#if YANFS_ROLE == YC_ROLE_WRITER
    yc_writer();
#elif YANFS_ROLE == YC_ROLE_READER
    yc_reader();
#else
    yc_corrupt();
#endif
}

/* The IRQ route below the CPU is the caller's business (0019): the device's
 * IRQ_ENABLE and the PLIC's enable / priority / threshold. mie.MEIE belongs to
 * the runtime. */
static void yc_configure_route(void)
{
    yan_os_transport_set_irq_enable(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_set_threshold(0);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
}

int main(void)
{
#if YANFS_ROLE == YC_ROLE_WRITER
    yc_puts("yanfs: writer on a freshly formatted image\r\n");
#elif YANFS_ROLE == YC_ROLE_READER
    yc_puts("yanfs: reader on the image the writer left\r\n");
#else
    yc_puts("yanfs: corrupt mount must be refused\r\n");
#endif
    if (!yan_os_transport_host_ready()) {
        yc_fail(YC_NO_CHANNEL);
    }
    if (yan_os_block_max_count() != 1u) {
        yc_fail(YC_MAX_COUNT);
    }
    yc_configure_route();
    if (yan_os_task_spawn(yc_fs_task, NULL) != YAN_OS_TASK_OK) {
        yc_fail(YC_SPAWN);
    }
    yan_os_sched_run();
    yc_fail(YC_SPAWN); /* the scheduler never returns */
}
