#define _POSIX_C_SOURCE 200809L
#include "host_disk.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* GNU ld wrappers inject failures at the real stdio boundary. Production code
 * has no test switches; only this test executable links the wrappers. */
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
size_t __real_fread(void *, size_t, size_t, FILE *);
int __real_fflush(FILE *);
int __real_fclose(FILE *);
static FILE *fault_file;
static int short_write, flush_error, close_error;
static unsigned reads, writes, flushes;
static YanTransport transport;
static int published_early;

size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (file == fault_file) ++reads;
    return __real_fread(data, size, count, file);
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (file == fault_file) {
        ++writes;
        if (transport.h2g_head != 0) published_early = 1;
        if (short_write && count != 0) return __real_fwrite(data, size, count - 1, file);
    }
    return __real_fwrite(data, size, count, file);
}

int __wrap_fflush(FILE *file)
{
    if (file == fault_file) {
        ++flushes;
        if (transport.h2g_head != 0) published_early = 1;
        if (flush_error) return EOF;
    }
    return __real_fflush(file);
}

int __wrap_fclose(FILE *file)
{
    int fail = file == fault_file && close_error;
    int result = __real_fclose(file);
    return fail ? EOF : result;
}

static YanHostDisk disk;
static YanRam ram;
static YanHostBlock block;
static uint8_t data[4096], observed[4096];
static char image_path[64];

static void notify(void *context) { (void)context; }

void setUp(void)
{
    fault_file = NULL;
    short_write = flush_error = close_error = published_early = 0;
    reads = writes = flushes = 0;
    disk = (YanHostDisk){0};
    transport = (YanTransport){0};
    ram = (YanRam){0};
    block = (YanHostBlock){0};
    strcpy(image_path, "/tmp/yan-disk-test-XXXXXX");
    int fd = mkstemp(image_path);
    if (fd < 0 || ftruncate(fd, 8192) != 0 || close(fd) != 0) {
        fputs("HARNESS-ERROR: cannot create disk fixture\n", stderr);
        exit(2);
    }
    for (size_t i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(i * 19 + (i >> 8));
    if (!yan_host_disk_open(&disk, image_path) ||
        yan_ram_init(&ram, 16384) != YAN_OK ||
        yan_transport_configure(&transport, 0, 8192, 0, 16384) != YAN_OK ||
        yan_host_block_init_backend(&block, yan_host_disk_backend(&disk), 2) != YAN_OK) {
        fputs("HARNESS-ERROR: cannot initialize disk fixture\n", stderr);
        exit(2);
    }
    fault_file = disk.file;
    yan_transport_set_notify(&transport, notify, NULL);
}

void tearDown(void)
{
    fault_file = NULL;
    (void)yan_host_disk_close(&disk);
    yan_ram_destroy(&ram);
    (void)unlink(image_path);
}

static void request(uint8_t op)
{
    memset(ram.data, 0, ram.size);
    ram.data[0] = op;
    ram.data[2] = 0x34;
    ram.data[3] = 0x12;
    ram.data[4] = 1; /* LBA 1 */
    ram.data[8] = 1; /* count 1 */
    if (op == 2) memcpy(ram.data + 16, data, sizeof data);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_write(&transport,
        YAN_TRANSPORT_G2H_HEAD, op == 2 ? 4112 : 16));
}

static void error_header_only(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, yan_host_block_service(&block, &transport, &ram, 0));
    TEST_ASSERT_EQUAL_UINT32(16, transport.h2g_head);
    TEST_ASSERT_EQUAL_UINT8(2, ram.data[8193]);
    TEST_ASSERT_EQUAL_UINT8(0x34, ram.data[8194]);
    TEST_ASSERT_EQUAL_UINT8(0x12, ram.data[8195]);
    for (size_t i = 8; i < 16; ++i) TEST_ASSERT_EQUAL_UINT8(0, ram.data[8192 + i]);
    TEST_ASSERT_EQUAL_UINT32(1, block.served);
}

static void successful_write_is_visible_before_close(void)
{
    request(2);
    TEST_ASSERT_EQUAL_UINT32(1, yan_host_block_service(&block, &transport, &ram, 0));
    TEST_ASSERT_EQUAL_UINT8(0, ram.data[8193]);
    TEST_ASSERT_EQUAL_UINT8(1, ram.data[8200]);
    TEST_ASSERT_EQUAL_UINT32(1, writes);
    TEST_ASSERT_EQUAL_UINT32(1, flushes);
    TEST_ASSERT_FALSE(published_early);
    FILE *reader = fopen(image_path, "rb");
    TEST_ASSERT_NOT_NULL(reader);
    int seek = fseek(reader, 4096, SEEK_SET);
    size_t got = fread(observed, 1, sizeof observed, reader);
    int closed = fclose(reader);
    TEST_ASSERT_EQUAL_INT(0, seek);
    TEST_ASSERT_EQUAL_UINT32(sizeof observed, got);
    TEST_ASSERT_EQUAL_INT(0, closed);
    TEST_ASSERT_EQUAL_MEMORY(data, observed, sizeof data);
}

static void short_write_is_device_failure(void)
{
    short_write = 1;
    request(2);
    error_header_only();
    TEST_ASSERT_TRUE(disk.failed);
    TEST_ASSERT_EQUAL_UINT32(1, writes);
    TEST_ASSERT_EQUAL_UINT32(0, flushes);
}

static void read_only_stream_rejects_write(void)
{
    TEST_ASSERT_TRUE(yan_host_disk_close(&disk));
    disk.file = fopen(image_path, "rb");
    disk.bytes = 8192;
    TEST_ASSERT_NOT_NULL(disk.file);
    fault_file = disk.file;
    request(2);
    error_header_only();
    TEST_ASSERT_TRUE(disk.failed);
    TEST_ASSERT_EQUAL_UINT32(1, writes);
    TEST_ASSERT_EQUAL_UINT32(0, flushes);
}

static void flush_failure_never_acknowledges_success(void)
{
    flush_error = 1;
    request(2);
    error_header_only();
    TEST_ASSERT_EQUAL_UINT32(1, flushes);
    TEST_ASSERT_FALSE(published_early);
    flush_error = 0;
    YanHostBlockBackend backend = yan_host_disk_backend(&disk);
    TEST_ASSERT_FALSE(backend.write(backend.context, 4096, data, sizeof data));
    TEST_ASSERT_EQUAL_UINT32(1, writes); /* failed backend stays failed */
}

static void short_read_never_publishes_partial_payload(void)
{
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fileno(disk.file), 4100));
    request(1);
    memset(ram.data + 8192 + 16, 0xa5, 4096);
    error_header_only();
    TEST_ASSERT_TRUE(disk.failed);
    for (size_t i = 0; i < 4096; ++i) TEST_ASSERT_EQUAL_UINT8(0xa5, ram.data[8208 + i]);
}

static void failed_backend_rejects_reads_and_writes(void)
{
    flush_error = 1;
    request(2);
    error_header_only();
    TEST_ASSERT_TRUE(disk.failed);
    flush_error = 0;

    YanHostBlockBackend backend = yan_host_disk_backend(&disk);
    memset(observed, 0xa5, sizeof observed);
    TEST_ASSERT_FALSE(backend.read(backend.context, 4096, observed, sizeof observed));
    TEST_ASSERT_FALSE(backend.write(backend.context, 4096, data, sizeof data));
    TEST_ASSERT_EQUAL_UINT32(0, reads);
    TEST_ASSERT_EQUAL_UINT32(1, writes);
    TEST_ASSERT_EQUAL_UINT32(1, flushes);
    for (size_t i = 0; i < sizeof observed; ++i) TEST_ASSERT_EQUAL_UINT8(0xa5, observed[i]);
}

static void reopening_failed_backend_keeps_partial_write(void)
{
    short_write = 1;
    request(2);
    error_header_only();
    TEST_ASSERT_TRUE(disk.failed);
    TEST_ASSERT_TRUE(yan_host_disk_close(&disk));
    short_write = 0;
    TEST_ASSERT_TRUE(yan_host_disk_open(&disk, image_path));
    fault_file = disk.file;
    TEST_ASSERT_FALSE(disk.failed);

    /* The injected short write really transfers 4095 bytes. Closing may flush
     * them, and reopening neither rolls them back nor repairs the final byte. */
    YanHostBlockBackend backend = yan_host_disk_backend(&disk);
    TEST_ASSERT_TRUE(backend.read(backend.context, 4096, observed, sizeof observed));
    TEST_ASSERT_EQUAL_MEMORY(data, observed, sizeof data - 1);
    TEST_ASSERT_EQUAL_UINT8(0, observed[sizeof observed - 1]);
    TEST_ASSERT_TRUE(backend.read(backend.context, 0, observed, sizeof observed));
    for (size_t i = 0; i < sizeof observed; ++i) TEST_ASSERT_EQUAL_UINT8(0, observed[i]);

    TEST_ASSERT_TRUE(backend.write(backend.context, 4096, data, sizeof data));
    TEST_ASSERT_TRUE(backend.read(backend.context, 4096, observed, sizeof observed));
    TEST_ASSERT_EQUAL_MEMORY(data, observed, sizeof data);
}

static void backpressure_performs_no_file_io(void)
{
    request(2);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_host_publish(&transport, 8191));
    TEST_ASSERT_EQUAL_UINT32(0, yan_host_block_service(&block, &transport, &ram, 0));
    TEST_ASSERT_EQUAL_UINT32(0, transport.g2h_tail);
    TEST_ASSERT_EQUAL_UINT32(0, writes);
    TEST_ASSERT_EQUAL_UINT32(0, flushes);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_transport_write(&transport, YAN_TRANSPORT_H2G_TAIL, 8191));
    TEST_ASSERT_EQUAL_UINT32(1, yan_host_block_service(&block, &transport, &ram, 0));
    TEST_ASSERT_EQUAL_UINT32(1, writes);
    TEST_ASSERT_EQUAL_UINT32(1, flushes);
}

static void out_of_range_never_touches_file(void)
{
    request(2);
    ram.data[4] = 2;
    TEST_ASSERT_EQUAL_UINT32(1, yan_host_block_service(&block, &transport, &ram, 0));
    TEST_ASSERT_EQUAL_UINT8(1, ram.data[8193]);
    TEST_ASSERT_EQUAL_UINT32(0, writes);
}

static void close_failure_is_reported_and_object_reusable(void)
{
    close_error = 1;
    TEST_ASSERT_FALSE(yan_host_disk_close(&disk));
    TEST_ASSERT_NULL(disk.file);
    fault_file = NULL;
    TEST_ASSERT_TRUE(yan_host_disk_open(&disk, image_path));
    TEST_ASSERT_EQUAL_UINT64(8192, disk.bytes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(successful_write_is_visible_before_close);
    RUN_TEST(short_write_is_device_failure);
    RUN_TEST(read_only_stream_rejects_write);
    RUN_TEST(flush_failure_never_acknowledges_success);
    RUN_TEST(short_read_never_publishes_partial_payload);
    RUN_TEST(failed_backend_rejects_reads_and_writes);
    RUN_TEST(reopening_failed_backend_keeps_partial_write);
    RUN_TEST(backpressure_performs_no_file_io);
    RUN_TEST(out_of_range_never_touches_file);
    RUN_TEST(close_failure_is_reported_and_object_reusable);
    return UNITY_END();
}
