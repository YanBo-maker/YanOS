#define _POSIX_C_SOURCE 200809L

#include "note_write.h"
#include "fs_test_support.h"
#include "unity.h"
#include "yanfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char directory[256];
static char image[320];
static char lock_path[352];

static int fail_write;
static int fail_pwrite;
static int fail_regular_fsync;
static int fail_directory_fsync;
static int fail_rename;

ssize_t __real_write(int fd, const void *bytes, size_t length);
ssize_t __wrap_write(int fd, const void *bytes, size_t length)
{
    if (fail_write) {
        fail_write = 0;
        errno = EIO;
        return -1;
    }
    return __real_write(fd, bytes, length);
}

ssize_t __real_pwrite(int fd, const void *bytes, size_t length, off_t offset);
ssize_t __wrap_pwrite(int fd, const void *bytes, size_t length, off_t offset)
{
    if (fail_pwrite) {
        fail_pwrite = 0;
        errno = EIO;
        return -1;
    }
    return __real_pwrite(fd, bytes, length, offset);
}

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    struct stat info;
    if (fstat(fd, &info) != 0) {
        return __real_fsync(fd);
    }
    if (S_ISDIR(info.st_mode) && fail_directory_fsync) {
        fail_directory_fsync = 0;
        errno = EIO;
        return -1;
    }
    if (S_ISREG(info.st_mode) && fail_regular_fsync) {
        fail_regular_fsync = 0;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    if (fail_rename) {
        fail_rename = 0;
        errno = EIO;
        return -1;
    }
    return __real_rename(from, to);
}

typedef struct {
    uint8_t *bytes;
    size_t size;
} Snapshot;

typedef struct {
    int fd;
    uint32_t blocks;
    YanFs fs;
} MountedImage;

static void write_all(int fd, const void *bytes, size_t length)
{
    const uint8_t *at = bytes;
    while (length != 0) {
        ssize_t done = __real_write(fd, at, length);
        TEST_ASSERT_GREATER_THAN_INT(0, done);
        at += (size_t)done;
        length -= (size_t)done;
    }
}

static void make_image(uint32_t blocks)
{
    uint8_t *bytes = calloc(blocks, YAN_FS_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_format_metadata(bytes, blocks));
    int fd = open(image, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    write_all(fd, bytes, (size_t)blocks * YAN_FS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    free(bytes);
}

static Snapshot snapshot_image(void)
{
    Snapshot snapshot = {0};
    int fd = open(image, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, fstat(fd, &info));
    snapshot.size = (size_t)info.st_size;
    snapshot.bytes = malloc(snapshot.size);
    TEST_ASSERT_NOT_NULL(snapshot.bytes);
    size_t offset = 0;
    while (offset < snapshot.size) {
        ssize_t got = read(fd, snapshot.bytes + offset, snapshot.size - offset);
        TEST_ASSERT_GREATER_THAN_INT(0, got);
        offset += (size_t)got;
    }
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    return snapshot;
}

static void assert_image_matches(const Snapshot *snapshot)
{
    int fd = open(image, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, fstat(fd, &info));
    TEST_ASSERT_EQUAL_UINT(snapshot->size, (size_t)info.st_size);
    uint8_t *bytes = malloc(snapshot->size);
    TEST_ASSERT_NOT_NULL(bytes);
    size_t offset = 0;
    while (offset < snapshot->size) {
        ssize_t got = read(fd, bytes + offset, snapshot->size - offset);
        TEST_ASSERT_GREATER_THAN_INT(0, got);
        offset += (size_t)got;
    }
    TEST_ASSERT_EQUAL_MEMORY(snapshot->bytes, bytes, snapshot->size);
    free(bytes);
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

static void drop_snapshot(Snapshot *snapshot)
{
    free(snapshot->bytes);
    snapshot->bytes = NULL;
    snapshot->size = 0;
}

static YanFsIoResult mounted_capacity(void *context, uint64_t *blocks)
{
    const MountedImage *mounted = context;
    *blocks = mounted->blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult mounted_read(void *context, uint32_t lba, uint8_t out[4096])
{
    const MountedImage *mounted = context;
    return pread(mounted->fd, out, YAN_FS_BLOCK_SIZE,
                 (off_t)lba * YAN_FS_BLOCK_SIZE) == (ssize_t)YAN_FS_BLOCK_SIZE
               ? YAN_FS_IO_OK : YAN_FS_IO_ERROR;
}

static YanFsIoResult mounted_write(void *context, uint32_t lba, const uint8_t in[4096])
{
    (void)context;
    (void)lba;
    (void)in;
    return YAN_FS_IO_ERROR;
}

static void assert_note_bytes(const char *name, const uint8_t *bytes, uint32_t length)
{
    MountedImage mounted = {0};
    mounted.fd = open(image, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, mounted.fd);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, fstat(mounted.fd, &info));
    mounted.blocks = (uint32_t)((size_t)info.st_size / YAN_FS_BLOCK_SIZE);
    YanFsBlockIo io = {&mounted, mounted_capacity, mounted_read, mounted_write};
    TEST_ASSERT_EQUAL(YAN_FS_OK, test_fs_init(&mounted.fs, io));
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_mount(&mounted.fs));
    YanFsInfo note;
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_stat(&mounted.fs, name, &note));
    TEST_ASSERT_EQUAL_UINT32(length, note.size_bytes);
    uint8_t *read_back = malloc(length == 0 ? 1 : length);
    TEST_ASSERT_NOT_NULL(read_back);
    uint32_t got = 0;
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_fs_read(&mounted.fs, name, 0, read_back, length, &got));
    TEST_ASSERT_EQUAL_UINT32(length, got);
    TEST_ASSERT_EQUAL_MEMORY(bytes, read_back, length);
    free(read_back);
    TEST_ASSERT_EQUAL_INT(0, close(mounted.fd));
}

static int directory_entries(void)
{
    DIR *dir = opendir(directory);
    TEST_ASSERT_NOT_NULL(dir);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

static void assert_no_temporary(void)
{
    TEST_ASSERT_EQUAL_INT(2, directory_entries());
    TEST_ASSERT_EQUAL_INT(0, access(image, F_OK));
    TEST_ASSERT_EQUAL_INT(0, access(lock_path, F_OK));
}

static void arm_failure(void)
{
    fail_write = 0;
    fail_pwrite = 0;
    fail_regular_fsync = 0;
    fail_directory_fsync = 0;
    fail_rename = 0;
}

void setUp(void)
{
    snprintf(directory, sizeof directory, "/tmp/yan-note-write-faults-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
    snprintf(image, sizeof image, "%s/notes.img", directory);
    snprintf(lock_path, sizeof lock_path, "%s.lock", image);
    arm_failure();
    make_image(64);
}

void tearDown(void)
{
    DIR *dir = opendir(directory);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char path[512];
            snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(directory);
}

static void test_0035_R6_AC1_copy_failure(void)
{
    Snapshot before = snapshot_image();
    fail_write = 1;
    bool committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_IO,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    TEST_ASSERT_FALSE(fail_write);
    assert_image_matches(&before);
    assert_no_temporary();
    drop_snapshot(&before);
}

static void test_0035_R6_AC1_yanfs_write_failure(void)
{
    Snapshot before = snapshot_image();
    fail_pwrite = 1;
    bool committed = true;
    int result = yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                &committed);
    TEST_ASSERT_NOT_EQUAL(YAN_FS_OK, result);
    TEST_ASSERT_FALSE(committed);
    TEST_ASSERT_FALSE(fail_pwrite);
    assert_image_matches(&before);
    assert_no_temporary();
    drop_snapshot(&before);
}

static void test_0035_R6_AC1_copy_fsync_failure(void)
{
    Snapshot before = snapshot_image();
    fail_regular_fsync = 1;
    bool committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_IO,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    TEST_ASSERT_FALSE(fail_regular_fsync);
    assert_image_matches(&before);
    assert_no_temporary();
    drop_snapshot(&before);
}

static void test_0035_R6_AC1_rename_failure(void)
{
    Snapshot before = snapshot_image();
    fail_rename = 1;
    bool committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_IO,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    TEST_ASSERT_FALSE(fail_rename);
    assert_image_matches(&before);
    assert_no_temporary();
    drop_snapshot(&before);
}

static void test_0035_R6_AC3_directory_fsync_failure_is_committed(void)
{
    static const uint8_t text[] = "committed\n";
    fail_directory_fsync = 1;
    bool committed = false;
    TEST_ASSERT_EQUAL(YAN_FS_IO,
                      yan_note_write(image, false, "a.txt", text, sizeof text - 1,
                                     &committed));
    TEST_ASSERT_TRUE(committed);
    TEST_ASSERT_FALSE(fail_directory_fsync);
    assert_note_bytes("a.txt", text, sizeof text - 1);
    assert_no_temporary();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_0035_R6_AC1_copy_failure);
    RUN_TEST(test_0035_R6_AC1_yanfs_write_failure);
    RUN_TEST(test_0035_R6_AC1_copy_fsync_failure);
    RUN_TEST(test_0035_R6_AC1_rename_failure);
    RUN_TEST(test_0035_R6_AC3_directory_fsync_failure_is_committed);
    return UNITY_END();
}
