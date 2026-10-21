#ifndef YAN_TESTS_FS_TEST_SUPPORT_H
#define YAN_TESTS_FS_TEST_SUPPORT_H

/* Shared D=1 storage for the native and RV32 suites that drive the real YanFS
 * API. 0036-R3 removed the built-in directory array and every service's fixed
 * table, so a test that only needs the original 63-file images now supplies
 * one 4096-byte directory per YanFs and one 63-entry table per service.
 *
 * The helpers are `static inline` so a translation unit that includes this
 * header but uses only some of them stays warning-free under -Werror.
 * Contexts are keyed by their own address: a suite that keeps two filesystems
 * mounted at once gets two directories, and re-initializing the same context
 * reuses its storage instead of leaking a slot. */

#include "note_blocks.h"
#include "note_metadata.h"
#include "note_relations.h"
#include "search_terms_index.h"
#include "search_terms_linear.h"
#include "yanfs.h"

#define TEST_FS_STORAGE_SLOTS 32u
#define TEST_NOTE_STORAGE_SLOTS 8u
#define TEST_RELATION_STORAGE_SLOTS 8u
#define TEST_LINEAR_STORAGE_SLOTS 8u
#define TEST_INDEX_STORAGE_SLOTS 8u
#define TEST_BLOCKS_STORAGE_SLOTS 8u

typedef struct {
    YanFs *owner;
    uint8_t directory[YAN_FS_BLOCK_SIZE];
    uint16_t name_order[YAN_FS_MAX_FILES];
} TestFsStorage;

static TestFsStorage test_fs_storage[TEST_FS_STORAGE_SLOTS];
static uint32_t test_fs_storage_used;

static inline YanFsResult test_fs_init(YanFs *fs, YanFsBlockIo io)
{
    for (uint32_t i = 0; i < test_fs_storage_used; ++i) {
        if (test_fs_storage[i].owner == fs) {
            return yan_fs_init_directory(fs, io, test_fs_storage[i].directory,
                                         1u,
                                         test_fs_storage[i].name_order);
        }
    }
    if (test_fs_storage_used == TEST_FS_STORAGE_SLOTS) {
        return YAN_FS_INVALID;
    }
    TestFsStorage *slot = &test_fs_storage[test_fs_storage_used++];
    slot->owner = fs;
    return yan_fs_init_directory(fs, io, slot->directory, 1u,
                                 slot->name_order);
}

/* A D=1 mount is the only shape these helpers support; a multi-block image
 * must allocate its own tables from the test's capacity. */
static inline bool test_fs_is_v1(const YanFs *fs)
{
    return fs != NULL && fs->entry_capacity == YAN_FS_MAX_FILES;
}

/* The term backends accept an uninitialized filesystem so status can report
 * UNINITIALIZED; the test table is still 63 entries and is never indexed when
 * the source has no directory yet. */
static inline bool test_fs_is_term_source(const YanFs *fs)
{
    return fs != NULL &&
           (!fs->initialized || fs->entry_capacity == YAN_FS_MAX_FILES);
}

typedef struct {
    YanNoteContext *owner;
    YanNoteEntry entries[YAN_FS_MAX_FILES];
    char sort_name[YAN_FS_MAX_FILES][32];
    uint16_t sort_index[YAN_FS_MAX_FILES];
} TestNoteStorage;

static TestNoteStorage test_note_storage[TEST_NOTE_STORAGE_SLOTS];
static uint32_t test_note_storage_used;

static inline YanNoteResult test_note_init(YanNoteContext *context, YanFs *fs)
{
    if (!test_fs_is_v1(fs)) return YAN_NOTE_INVALID;
    for (uint32_t i = 0; i < test_note_storage_used; ++i) {
        if (test_note_storage[i].owner == context) {
            return yan_note_init(context, fs, test_note_storage[i].entries,
                                 test_note_storage[i].sort_name,
                                 test_note_storage[i].sort_index);
        }
    }
    if (test_note_storage_used == TEST_NOTE_STORAGE_SLOTS) {
        return YAN_NOTE_INVALID;
    }
    TestNoteStorage *slot = &test_note_storage[test_note_storage_used++];
    slot->owner = context;
    return yan_note_init(context, fs, slot->entries, slot->sort_name,
                         slot->sort_index);
}

typedef struct {
    YanRelationContext *owner;
    YanRelationRow rows[YAN_FS_MAX_FILES];
    uint16_t sort_index[YAN_FS_MAX_FILES];
} TestRelationStorage;

static TestRelationStorage test_relation_storage[TEST_RELATION_STORAGE_SLOTS];
static uint32_t test_relation_storage_used;

static inline YanNoteResult test_relation_init(YanRelationContext *context,
                                               YanFs *fs)
{
    if (!test_fs_is_v1(fs)) return YAN_NOTE_INVALID;
    for (uint32_t i = 0; i < test_relation_storage_used; ++i) {
        if (test_relation_storage[i].owner == context) {
            return yan_relation_init(context, fs,
                                     test_relation_storage[i].rows,
                                     test_relation_storage[i].sort_index);
        }
    }
    if (test_relation_storage_used == TEST_RELATION_STORAGE_SLOTS) {
        return YAN_NOTE_INVALID;
    }
    TestRelationStorage *slot = &test_relation_storage[test_relation_storage_used++];
    slot->owner = context;
    return yan_relation_init(context, fs, slot->rows, slot->sort_index);
}

typedef struct {
    YanBlockContext *owner;
    char names[YAN_FS_MAX_FILES][32];
    uint16_t sort_index[YAN_FS_MAX_FILES];
} TestBlocksStorage;

static TestBlocksStorage test_blocks_storage[TEST_BLOCKS_STORAGE_SLOTS];
static uint32_t test_blocks_storage_used;

static inline YanBlockResult test_blocks_init(YanBlockContext *context,
                                              YanFs *fs)
{
    if (!test_fs_is_v1(fs)) return YAN_BLOCK_INVALID;
    for (uint32_t i = 0; i < test_blocks_storage_used; ++i) {
        if (test_blocks_storage[i].owner == context) {
            return yan_blocks_init(context, fs,
                                   test_blocks_storage[i].names,
                                   test_blocks_storage[i].sort_index);
        }
    }
    if (test_blocks_storage_used == TEST_BLOCKS_STORAGE_SLOTS) {
        return YAN_BLOCK_INVALID;
    }
    TestBlocksStorage *slot = &test_blocks_storage[test_blocks_storage_used++];
    slot->owner = context;
    return yan_blocks_init(context, fs, slot->names, slot->sort_index);
}

typedef struct {
    YanSearchTermsLinear *owner;
    uint8_t eligible[YAN_FS_MAX_FILES];
} TestLinearStorage;

static TestLinearStorage test_linear_storage[TEST_LINEAR_STORAGE_SLOTS];
static uint32_t test_linear_storage_used;

static inline YanSearchTermsResult test_terms_linear_init(
    YanSearchTermsLinear *linear, YanFs *fs)
{
    if (!test_fs_is_term_source(fs)) return YAN_SEARCH_TERMS_INVALID;
    for (uint32_t i = 0; i < test_linear_storage_used; ++i) {
        if (test_linear_storage[i].owner == linear) {
            return yan_search_terms_linear_init(
                linear, fs, test_linear_storage[i].eligible);
        }
    }
    if (test_linear_storage_used == TEST_LINEAR_STORAGE_SLOTS) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    TestLinearStorage *slot = &test_linear_storage[test_linear_storage_used++];
    slot->owner = linear;
    return yan_search_terms_linear_init(linear, fs, slot->eligible);
}

typedef struct {
    YanSearchTermsIndex *owner;
    YanFsInfo entries[YAN_FS_MAX_FILES];
    uint8_t eligible[YAN_FS_MAX_FILES];
} TestIndexStorage;

static TestIndexStorage test_index_storage[TEST_INDEX_STORAGE_SLOTS];
static uint32_t test_index_storage_used;

static inline YanSearchTermsResult test_terms_index_init(
    YanSearchTermsIndex *index, YanFs *fs)
{
    if (!test_fs_is_term_source(fs)) return YAN_SEARCH_TERMS_INVALID;
    for (uint32_t i = 0; i < test_index_storage_used; ++i) {
        if (test_index_storage[i].owner == index) {
            return yan_search_terms_index_init(
                index, fs, test_index_storage[i].entries,
                test_index_storage[i].eligible);
        }
    }
    if (test_index_storage_used == TEST_INDEX_STORAGE_SLOTS) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    TestIndexStorage *slot = &test_index_storage[test_index_storage_used++];
    slot->owner = index;
    return yan_search_terms_index_init(index, fs, slot->entries,
                                       slot->eligible);
}

#endif
