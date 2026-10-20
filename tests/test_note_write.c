#define _POSIX_C_SOURCE 200809L

#include "note_write.h"
#include "fs_test_support.h"
#include "semantic_posix.h"
#include "unity.h"
#include "yanfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char directory[256];
static char image[320];
static char lock_path[352];

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
        ssize_t done = write(fd, at, length);
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
    const MountedImage *mounted = context;
    return pwrite(mounted->fd, in, YAN_FS_BLOCK_SIZE,
                  (off_t)lba * YAN_FS_BLOCK_SIZE) == (ssize_t)YAN_FS_BLOCK_SIZE
               ? YAN_FS_IO_OK : YAN_FS_IO_ERROR;
}

static void mounted_open(MountedImage *mounted, int flags)
{
    memset(mounted, 0, sizeof *mounted);
    mounted->fd = open(image, flags | O_CLOEXEC);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, mounted->fd);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, fstat(mounted->fd, &info));
    mounted->blocks = (uint32_t)((size_t)info.st_size / YAN_FS_BLOCK_SIZE);
    YanFsBlockIo io = {mounted, mounted_capacity, mounted_read, mounted_write};
    TEST_ASSERT_EQUAL(YAN_FS_OK, test_fs_init(&mounted->fs, io));
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_mount(&mounted->fs));
}

static void mounted_close(MountedImage *mounted)
{
    TEST_ASSERT_EQUAL_INT(0, close(mounted->fd));
}

static void assert_note_bytes(const char *name, const uint8_t *bytes, uint32_t length)
{
    MountedImage mounted;
    mounted_open(&mounted, O_RDONLY);
    YanFsInfo info;
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_stat(&mounted.fs, name, &info));
    TEST_ASSERT_EQUAL_UINT32(length, info.size_bytes);
    uint8_t *read_back = malloc(length == 0 ? 1 : length);
    TEST_ASSERT_NOT_NULL(read_back);
    uint32_t got = 0;
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_read(&mounted.fs, name, 0, read_back, length, &got));
    TEST_ASSERT_EQUAL_UINT32(length, got);
    TEST_ASSERT_EQUAL_MEMORY(bytes, read_back, length);
    free(read_back);
    mounted_close(&mounted);
}

static void fill_directory(void)
{
    MountedImage mounted;
    mounted_open(&mounted, O_RDWR);
    /* a.txt is already present, so 62 more fill the 63 entries. */
    for (uint32_t i = 0; i < YAN_FS_MAX_FILES - 1; ++i) {
        char name[16];
        snprintf(name, sizeof name, "f%02u", i);
        TEST_ASSERT_EQUAL(YAN_FS_OK,
                          yan_fs_create(&mounted.fs, name, (const uint8_t *)"x", 1));
    }
    mounted_close(&mounted);
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

static void assert_only_image_and_lock(void)
{
    TEST_ASSERT_EQUAL_INT(2, directory_entries());
    TEST_ASSERT_EQUAL_INT(0, access(image, F_OK));
    TEST_ASSERT_EQUAL_INT(0, access(lock_path, F_OK));
}

static pid_t child_holding_lock(void)
{
    int pipe_fds[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipe_fds));
    pid_t child = fork();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, child);
    if (child == 0) {
        close(pipe_fds[0]);
        int fd = -1;
        if (yan_image_lock(image, LOCK_EX, &fd) != YAN_FS_OK) {
            _exit(1);
        }
        char ready = 'x';
        if (write(pipe_fds[1], &ready, 1) != 1) {
            _exit(1);
        }
        for (;;) {
            pause();
        }
    }
    close(pipe_fds[1]);
    char ready = 0;
    TEST_ASSERT_EQUAL_INT(1, read(pipe_fds[0], &ready, 1));
    TEST_ASSERT_EQUAL_INT('x', ready);
    close(pipe_fds[0]);
    return child;
}

static void kill_child(pid_t child)
{
    TEST_ASSERT_EQUAL_INT(0, kill(child, SIGKILL));
    int status = 0;
    TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFSIGNALED(status));
}

static void assert_invalid_input(const char *name, const uint8_t *text, uint32_t length)
{
    char missing[352];
    char missing_lock[384];
    snprintf(missing, sizeof missing, "%s/missing.img", directory);
    snprintf(missing_lock, sizeof missing_lock, "%s.lock", missing);
    bool committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_INVALID,
                      yan_note_write(missing, false, name, text, length, &committed));
    TEST_ASSERT_FALSE(committed);
    TEST_ASSERT_EQUAL_INT(-1, access(missing, F_OK));
    TEST_ASSERT_EQUAL_INT(-1, access(missing_lock, F_OK));
}

void setUp(void)
{
    snprintf(directory, sizeof directory, "/tmp/yan-note-write-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
    snprintf(image, sizeof image, "%s/notes.img", directory);
    snprintf(lock_path, sizeof lock_path, "%s.lock", image);
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

static void test_0035_R5_AC1_exclusive_lock_returns_busy_without_writing(void)
{
    make_image(64);
    Snapshot before = snapshot_image();
    pid_t child = child_holding_lock();
    bool committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_BUSY,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    assert_image_matches(&before);
    assert_only_image_and_lock();
    kill_child(child);
    drop_snapshot(&before);
}

static void test_0035_R5_AC4_killed_holder_releases_lock(void)
{
    make_image(64);
    pid_t child = child_holding_lock();
    kill_child(child);
    bool committed = false;
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                     &committed));
    TEST_ASSERT_TRUE(committed);
    assert_note_bytes("a.txt", (const uint8_t *)"a\n", 2);
}

static void test_0035_R5_shared_and_exclusive_modes(void)
{
    make_image(64);
    int shared_a = -1;
    int shared_b = -1;
    int exclusive = -1;
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_image_lock(image, LOCK_SH, &shared_a));
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_image_lock(image, LOCK_SH, &shared_b));
    TEST_ASSERT_EQUAL(YAN_FS_BUSY, yan_image_lock(image, LOCK_EX, &exclusive));
    TEST_ASSERT_EQUAL_INT(-1, exclusive);
    TEST_ASSERT_EQUAL_INT(0, close(shared_a));
    TEST_ASSERT_EQUAL_INT(0, close(shared_b));

    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_image_lock(image, LOCK_EX, &exclusive));
    shared_a = -1;
    TEST_ASSERT_EQUAL(YAN_FS_BUSY, yan_image_lock(image, LOCK_SH, &shared_a));
    TEST_ASSERT_EQUAL_INT(-1, shared_a);
    TEST_ASSERT_EQUAL_INT(0, close(exclusive));
}

static void test_0035_R5_AC5_readonly_path_ignores_exclusive_lock(void)
{
    make_image(64);
    MountedImage mounted;
    mounted_open(&mounted, O_RDWR);
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_fs_create(&mounted.fs, "note", (const uint8_t *)"notes\n", 6));
    mounted_close(&mounted);
    int lock_fd = -1;
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_image_lock(image, LOCK_EX, &lock_fd));

    YanSemanticPosix adapter = {0};
    adapter.image_fd = -1;
    adapter.index_fd = -1;
    TEST_ASSERT_EQUAL(YAN_IMAGE_OPEN_OK, yan_semantic_posix_open_image_ex(&adapter, image));
    YanSemanticSource source;
    yan_semantic_posix_bind_source(&adapter, &source);
    uint8_t digest[32];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, digest));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(0, close(lock_fd));
}

static void test_0035_R6_AC1_create_and_replace_read_back(void)
{
    make_image(64);
    static const uint8_t first[] = "a\n";
    static const uint8_t second[] = "\tfirst\r\nsecond\n";
    bool committed = false;
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_note_write(image, false, "a.txt", first, sizeof first - 1,
                                     &committed));
    TEST_ASSERT_TRUE(committed);
    assert_note_bytes("a.txt", first, sizeof first - 1);

    committed = false;
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_note_write(image, true, "a.txt", second, sizeof second - 1,
                                     &committed));
    TEST_ASSERT_TRUE(committed);
    assert_note_bytes("a.txt", second, sizeof second - 1);
    assert_only_image_and_lock();
}

static void test_0035_R6_AC1_yanfs_errors_pass_through(void)
{
    make_image(256);
    static const uint8_t text[] = "text\n";

    bool committed = false;
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_note_write(image, false, "a.txt", text, sizeof text - 1,
                                     &committed));
    Snapshot before = snapshot_image();
    committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_EXISTS,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"other\n", 6,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    assert_image_matches(&before);
    drop_snapshot(&before);

    before = snapshot_image();
    committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_NOT_FOUND,
                      yan_note_write(image, true, "missing.txt", text, sizeof text - 1,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    assert_image_matches(&before);
    drop_snapshot(&before);

    fill_directory();
    before = snapshot_image();
    committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_DIRECTORY_FULL,
                      yan_note_write(image, false, "full.txt", text, sizeof text - 1,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    assert_image_matches(&before);
    drop_snapshot(&before);
}

static void test_0035_R6_AC2_corrupt_image_is_not_repaired(void)
{
    make_image(64);
    int fd = open(image, O_RDWR | O_CLOEXEC);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL_INT(1, pread(fd, &byte, 1, 60));
    byte ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(1, pwrite(fd, &byte, 1, 60));
    TEST_ASSERT_EQUAL_INT(0, fsync(fd));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    Snapshot before = snapshot_image();
    bool committed = true;
    TEST_ASSERT_EQUAL(YAN_FS_CORRUPT,
                      yan_note_write(image, false, "a.txt", (const uint8_t *)"a\n", 2,
                                     &committed));
    TEST_ASSERT_FALSE(committed);
    assert_image_matches(&before);
    assert_only_image_and_lock();
    drop_snapshot(&before);
}

static void test_0035_R7_AC1_bad_input_is_rejected_before_the_image(void)
{
    static const uint8_t with_nul[] = {'a', 0, 'b'};
    static const uint8_t bare_cr[] = {'a', '\r', 'b'};
    static const uint8_t bad_utf8[] = {0xC0, 0x80};
    static uint8_t oversized[16385];
    memset(oversized, 'a', sizeof oversized);

    assert_invalid_input("a.txt", with_nul, sizeof with_nul);
    assert_invalid_input("a.txt", bare_cr, sizeof bare_cr);
    assert_invalid_input("a.txt", bad_utf8, sizeof bad_utf8);
    assert_invalid_input("a.txt", oversized, sizeof oversized);
    assert_invalid_input("bad/name", (const uint8_t *)"a\n", 2);
    assert_invalid_input("..", (const uint8_t *)"a\n", 2);

    static const uint8_t good[] = "a\r\n\tb";
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_note_write_check("ok.txt", good, sizeof good - 1));
    TEST_ASSERT_EQUAL(YAN_FS_OK,
                      yan_note_write_check("ok.txt", oversized, sizeof oversized - 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_0035_R5_AC1_exclusive_lock_returns_busy_without_writing);
    RUN_TEST(test_0035_R5_AC4_killed_holder_releases_lock);
    RUN_TEST(test_0035_R5_shared_and_exclusive_modes);
    RUN_TEST(test_0035_R5_AC5_readonly_path_ignores_exclusive_lock);
    RUN_TEST(test_0035_R6_AC1_create_and_replace_read_back);
    RUN_TEST(test_0035_R6_AC1_yanfs_errors_pass_through);
    RUN_TEST(test_0035_R6_AC2_corrupt_image_is_not_repaired);
    RUN_TEST(test_0035_R7_AC1_bad_input_is_rejected_before_the_image);
    return UNITY_END();
}
