#define _POSIX_C_SOURCE 200809L
#include "semantic_posix.h"
#include "unity.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char directory[] = "/tmp/yan-semantic-test-XXXXXX";
static char image[256], index_path[256], model[256], runner[256], alias[256];
static uint8_t disk[16 * 4096];
static YanFs writer;
static YanSemanticPosix adapter;
static YanSemanticIdentity identity;
static YanSemanticSource source;
static YanSemanticStore store;

static YanFsIoResult cap(void *p, uint64_t *blocks)
{ (void)p; *blocks = 16; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *p, uint32_t lba, uint8_t *out)
{ (void)p; if (lba >= 16) return YAN_FS_IO_ERROR; memcpy(out, disk + lba * 4096, 4096); return YAN_FS_IO_OK; }
static YanFsIoResult write_block(void *p, uint32_t lba, const uint8_t *in)
{ (void)p; if (lba >= 16) return YAN_FS_IO_ERROR; memcpy(disk + lba * 4096, in, 4096); return YAN_FS_IO_OK; }
static void write_file(const char *path, const uint8_t *bytes, size_t n)
{
    FILE *f = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT(n, fwrite(bytes, 1, n, f)); TEST_ASSERT_EQUAL_INT(0, fclose(f));
}
static void assert_file(const char *path, const uint8_t *bytes, size_t n)
{
    uint8_t chunk[4096]; FILE *f = fopen(path, "rb"); TEST_ASSERT_NOT_NULL(f);
    for (size_t i = 0; i < n;) {
        size_t amount = n - i; if (amount > sizeof chunk) amount = sizeof chunk;
        TEST_ASSERT_EQUAL_UINT(amount, fread(chunk, 1, amount, f));
        TEST_ASSERT_EQUAL_MEMORY(bytes + i, chunk, amount); i += amount;
    }
    TEST_ASSERT_EQUAL_INT(EOF, fgetc(f)); TEST_ASSERT_EQUAL_INT(0, fclose(f));
}
static YanSemanticResult open_adapter(void)
{ return yan_semantic_posix_open(&adapter, image, index_path, model, runner, &identity); }
void setUp(void)
{
    strcpy(directory, "/tmp/yan-semantic-test-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
    snprintf(image, sizeof image, "%s/source.img", directory);
    snprintf(index_path, sizeof index_path, "%s/index", directory);
    snprintf(model, sizeof model, "%s/model", directory);
    snprintf(runner, sizeof runner, "%s/runner", directory);
    snprintf(alias, sizeof alias, "%s/alias", directory);
    memset(disk, 0, sizeof disk);
    memset(&writer, 0, sizeof writer);
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_format_metadata(disk, 16));
    YanFsBlockIo io = {NULL, cap, read_block, write_block};
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_init(&writer, io));
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_mount(&writer));
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_create(&writer, "note", (const uint8_t *)"notes\n", 6));
    write_file(image, disk, sizeof disk);
    write_file(model, (const uint8_t *)"model", 5);
    write_file(runner, (const uint8_t *)"runner", 6);
    memset(&adapter, 0, sizeof adapter); adapter.image_fd = adapter.index_fd = -1;
}
void tearDown(void)
{
    yan_semantic_posix_cleanup(&adapter);
    unlink(alias); unlink(index_path); unlink(image); unlink(model); unlink(runner);
    rmdir(index_path); rmdir(directory);
}
static void test_readonly_source_and_independent_index(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, open_adapter());
    yan_semantic_posix_bind(&adapter, &source, &store);
    uint32_t cursor = 0; YanSemanticFile file; uint8_t bytes[6];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, source.next(source.context, &cursor, &file));
    TEST_ASSERT_EQUAL_STRING("note", file.name); TEST_ASSERT_EQUAL_UINT32(0, file.slot);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, source.read(source.context, file.name, 0, bytes, 6));
    TEST_ASSERT_EQUAL_MEMORY("notes\n", bytes, 6);
    TEST_ASSERT_EQUAL_INT(O_RDONLY, fcntl(adapter.image_fd, F_GETFL) & O_ACCMODE);
    TEST_ASSERT_EQUAL(YAN_FS_IO_ERROR, adapter.fs.io.write_block(adapter.fs.io.context, 0, disk));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.begin_write(store.context));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.write(store.context, (const uint8_t *)"new", 3));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.finish_write(store.context));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.publish(store.context));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    assert_file(image, disk, sizeof disk);
    assert_file(model, (const uint8_t *)"model", 5);
    assert_file(index_path, (const uint8_t *)"new", 3);
}
static void test_hardlink_and_symlink_input_aliases(void)
{
    TEST_ASSERT_EQUAL_INT(0, link(image, index_path));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, open_adapter());
    TEST_ASSERT_EQUAL_INT(0, unlink(index_path));
    TEST_ASSERT_EQUAL_INT(0, symlink(model, index_path));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, open_adapter());
    TEST_ASSERT_EQUAL_INT(0, unlink(index_path));
    TEST_ASSERT_EQUAL_INT(0, unlink(runner));
    TEST_ASSERT_EQUAL_INT(0, link(model, runner));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, open_adapter());
    assert_file(image, disk, sizeof disk);
}
static void test_discard_and_rename_failure_preserve_old(void)
{
    write_file(index_path, (const uint8_t *)"old", 3);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, open_adapter());
    yan_semantic_posix_bind(&adapter, &source, &store);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.begin_write(store.context));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.write(store.context, (const uint8_t *)"new", 3));
    store.discard(store.context);
    assert_file(index_path, (const uint8_t *)"old", 3);
    TEST_ASSERT_FALSE(adapter.owns_temporary);
    TEST_ASSERT_EQUAL_INT(0, unlink(index_path));
    TEST_ASSERT_EQUAL_INT(0, mkdir(index_path, 0700));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.begin_write(store.context));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, store.finish_write(store.context));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_IO, store.publish(store.context));
    char *temporary = strdup(adapter.temporary_path); TEST_ASSERT_NOT_NULL(temporary);
    store.discard(store.context);
    TEST_ASSERT_EQUAL_INT(-1, access(temporary, F_OK)); free(temporary);
    struct stat st; TEST_ASSERT_EQUAL_INT(0, stat(index_path, &st)); TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
}
static void test_missing_dependency_before_index_observation(void)
{
    TEST_ASSERT_EQUAL_INT(0, unlink(model));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_DEPENDENCY, open_adapter());
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, adapter.image_fd);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    assert_file(image, disk, sizeof disk);
}
static void test_one_block_empty_filesystem_is_legal(void)
{
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_format_metadata(disk, 1));
    write_file(image, disk, 4096);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, open_adapter());
    yan_semantic_posix_bind(&adapter, &source, &store);
    uint32_t cursor = 0; YanSemanticFile file;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_MISSING, source.next(source.context, &cursor, &file));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    assert_file(image, disk, 4096);
}
static YanSemanticCorpus image_corpus;
static void test_image_only_open_without_dependencies(void)
{
    TEST_ASSERT_EQUAL_INT(0, unlink(model));
    TEST_ASSERT_EQUAL_INT(0, unlink(runner));
    TEST_ASSERT_EQUAL_INT(-1, access(index_path, F_OK));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_EQUAL_INT(-1, adapter.index_fd);
    TEST_ASSERT_NULL(adapter.index_path);
    TEST_ASSERT_NULL(adapter.temporary);
    TEST_ASSERT_FALSE(adapter.owns_temporary);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)sizeof disk, adapter.image_size);
    TEST_ASSERT_EQUAL_INT(O_RDONLY, fcntl(adapter.image_fd, F_GETFL) & O_ACCMODE);
    yan_semantic_posix_bind_source(&adapter, &source);
    uint32_t cursor = 0; YanSemanticFile file; uint8_t bytes[6];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, source.next(source.context, &cursor, &file));
    TEST_ASSERT_EQUAL_STRING("note", file.name);
    TEST_ASSERT_EQUAL_UINT32(0, file.slot);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, source.read(source.context, file.name, 0, bytes, 6));
    TEST_ASSERT_EQUAL_MEMORY("notes\n", bytes, 6);
    TEST_ASSERT_EQUAL(YAN_FS_IO_ERROR,
        adapter.fs.io.write_block(adapter.fs.io.context, 0, disk));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);
    assert_file(image, disk, sizeof disk);
    /* The same absent dependencies still make the four-path open DEPENDENCY. */
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_DEPENDENCY, open_adapter());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
}
static void test_image_open_validation_and_geometry(void)
{
    char missing[256], not_a_file[256];
    snprintf(missing, sizeof missing, "%s/missing.img", directory);
    snprintf(not_a_file, sizeof not_a_file, "%s/not-a-file", directory);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_posix_open_image(NULL, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_posix_open_image(&adapter, NULL));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_posix_open_image(&adapter, ""));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_IO, yan_semantic_posix_open_image(&adapter, missing));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));

    write_file(image, disk, 100);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, adapter.image_fd);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);

    write_file(image, disk, 4097);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);

    TEST_ASSERT_EQUAL_INT(0, mkdir(not_a_file, 0700));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_posix_open_image(&adapter, not_a_file));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(0, rmdir(not_a_file));
}
static void test_image_digest_matches_corpus_scan(void)
{
    static const uint8_t binary[] = {0x00, 0xff, 0x80, 0x0a};
    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_create(&writer, "bin", binary, sizeof binary));
    write_file(image, disk, sizeof disk);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_open_image(&adapter, image));
    yan_semantic_posix_bind_source(&adapter, &source);
    uint8_t digest[32];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, digest));
    memset(&image_corpus, 0, sizeof image_corpus);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&image_corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(1, image_corpus.skipped);
    TEST_ASSERT_EQUAL_MEMORY(image_corpus.source_hash, digest, 32);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    assert_file(image, disk, sizeof disk);
}
static void test_image_digest_ignores_bad_header(void)
{
    uint8_t before[32], after[32];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_open_image(&adapter, image));
    yan_semantic_posix_bind_source(&adapter, &source);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, before));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));

    TEST_ASSERT_EQUAL(YAN_FS_OK, yan_fs_create(&writer, "bad",
        (const uint8_t *)"@yan-note 9\n", 12));
    write_file(image, disk, sizeof disk);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_open_image(&adapter, image));
    yan_semantic_posix_bind_source(&adapter, &source);
    memset(&image_corpus, 0, sizeof image_corpus);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_METADATA,
                      yan_semantic_corpus_scan(&image_corpus, &source));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, after));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(before, after, 32));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
}
static void test_cleanup_close_error_and_idempotence(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, adapter.image_fd);
    /* Artificial fault: close the owned descriptor behind the adapter so its
     * own close(2) returns EBADF. The test is single-threaded and opens no
     * descriptor in between, so the number cannot be reused. */
    TEST_ASSERT_EQUAL_INT(0, close(adapter.image_fd));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_IO, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);
}
static void test_typed_open_reports_unsupported_and_legacy_mapping(void)
{
    uint8_t saved[4096];
    memcpy(saved, disk, sizeof saved);
    disk[8] = 3; disk[9] = disk[10] = disk[11] = 0;
    uint32_t crc = yan_fs_metadata_crc(disk);
    disk[60] = (uint8_t)crc; disk[61] = (uint8_t)(crc >> 8);
    disk[62] = (uint8_t)(crc >> 16); disk[63] = (uint8_t)(crc >> 24);
    write_file(image, disk, sizeof disk);
    memset(&adapter, 0, sizeof adapter); adapter.image_fd = adapter.index_fd = -1;
    TEST_ASSERT_EQUAL_INT(YAN_IMAGE_OPEN_UNSUPPORTED,
        yan_semantic_posix_open_image_ex(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT,
        yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    memcpy(disk, saved, sizeof saved);

    write_file(image, disk, 100);
    TEST_ASSERT_EQUAL_INT(YAN_IMAGE_OPEN_CORRUPT,
        yan_semantic_posix_open_image_ex(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT,
        yan_semantic_posix_open_image(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));

    write_file(image, disk, 4097);
    TEST_ASSERT_EQUAL_INT(YAN_IMAGE_OPEN_CORRUPT,
        yan_semantic_posix_open_image_ex(&adapter, image));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));

    char missing[256], not_a_file[256];
    snprintf(missing, sizeof missing, "%s/missing.img", directory);
    snprintf(not_a_file, sizeof not_a_file, "%s/not-a-file", directory);
    TEST_ASSERT_EQUAL_INT(YAN_IMAGE_OPEN_IO,
        yan_semantic_posix_open_image_ex(&adapter, missing));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_IO,
        yan_semantic_posix_open_image(&adapter, missing));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(0, mkdir(not_a_file, 0700));
    TEST_ASSERT_EQUAL_INT(YAN_IMAGE_OPEN_CORRUPT,
        yan_semantic_posix_open_image_ex(&adapter, not_a_file));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_posix_cleanup(&adapter));
    TEST_ASSERT_EQUAL_INT(0, rmdir(not_a_file));

    TEST_ASSERT_EQUAL_INT(YAN_IMAGE_OPEN_INVALID,
        yan_semantic_posix_open_image_ex(&adapter, ""));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
        yan_semantic_posix_open_image(&adapter, ""));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_readonly_source_and_independent_index);
    RUN_TEST(test_hardlink_and_symlink_input_aliases);
    RUN_TEST(test_discard_and_rename_failure_preserve_old);
    RUN_TEST(test_missing_dependency_before_index_observation);
    RUN_TEST(test_one_block_empty_filesystem_is_legal);
    RUN_TEST(test_image_only_open_without_dependencies);
    RUN_TEST(test_image_open_validation_and_geometry);
    RUN_TEST(test_image_digest_matches_corpus_scan);
    RUN_TEST(test_image_digest_ignores_bad_header);
    RUN_TEST(test_cleanup_close_error_and_idempotence);
    RUN_TEST(test_typed_open_reports_unsupported_and_legacy_mapping);
    return UNITY_END();
}
