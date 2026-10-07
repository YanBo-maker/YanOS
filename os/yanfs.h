#ifndef YAN_OS_YANFS_H
#define YAN_OS_YANFS_H

#include <stdbool.h>
#include <stdint.h>

/* YanFS core. The contract for the format and the original file API is
 * docs/specs/0021-yanfs.md; this header repeats the constants, enums,
 * structures and entry points fixed by its "接口" section. 0024 extends the
 * public surface with yan_fs_rename and yan_fs_copy and adds no enum value and
 * no context field. 0026 adds the in-memory source identity (the source_token
 * and source_cacheable fields of YanFs, observed through YanFsSource and
 * yan_fs_source); it adds no enum value and no on-disk field.
 *
 * The core owns the on-disk format and the metadata cache. It reaches a block
 * device only through YanFsBlockIo, whose callbacks are synchronous: they may
 * block and yield to the cooperative scheduler, but one block operation has
 * completed before they return. The core performs no Host I/O, includes no
 * Host header and depends on no test code, so the same source compiles for the
 * Guest.
 *
 * YanFs is larger than 8 KiB because it carries the metadata and scratch
 * blocks, so it lives in static storage or another long-lived object; it does
 * not fit the 4 KiB task stack of 0019, and no entry point allocates a whole
 * block on the stack. */

#define YAN_FS_BLOCK_SIZE UINT32_C(4096)
#define YAN_FS_MAX_FILES UINT32_C(63)
#define YAN_FS_NAME_MAX UINT32_C(31)
#define YAN_FS_HEADER_SIZE UINT32_C(64)
#define YAN_FS_ENTRY_SIZE UINT32_C(64)
#define YAN_FS_VERSION UINT32_C(1)

typedef enum {
    YAN_FS_OK = 0, YAN_FS_END = 1, YAN_FS_INVALID = 2,
    YAN_FS_NOT_MOUNTED = 3, YAN_FS_FAULTED = 4, YAN_FS_BUSY = 5,
    YAN_FS_EXISTS = 6, YAN_FS_NOT_FOUND = 7, YAN_FS_DIRECTORY_FULL = 8,
    YAN_FS_NOSPACE = 9, YAN_FS_CORRUPT = 10, YAN_FS_UNSUPPORTED = 11,
    YAN_FS_IO = 12, YAN_FS_PROTOCOL = 13
} YanFsResult;

typedef enum {
    YAN_FS_IO_OK = 0, YAN_FS_IO_ERROR = 1, YAN_FS_IO_PROTOCOL = 2
} YanFsIoResult;

/* The state enum is separate from the result enum on purpose: the result
 * YAN_FS_FAULTED says what an operation returned, the state says how the
 * instance got there. */
typedef enum {
    YAN_FS_UNMOUNTED = 0, YAN_FS_MOUNTED = 1, YAN_FS_STATE_FAULTED = 2
} YanFsState;

typedef struct {
    void *context;
    YanFsIoResult (*capacity)(void *, uint64_t *blocks);
    YanFsIoResult (*read_block)(void *, uint32_t lba, uint8_t out[4096]);
    YanFsIoResult (*write_block)(void *, uint32_t lba, const uint8_t data[4096]);
} YanFsBlockIo;

typedef struct { char name[32]; uint32_t size_bytes; } YanFsInfo;

/* 0026 source identity observation. token names the in-memory source that a
 * cached term index was built against; cacheable says whether the global
 * allocator could hand out an identity at all. This is a read-only view of the
 * two context fields below: the on-disk format has no place for them and no
 * entry point ever writes them to a device. */
typedef struct {
    YanFsState state;
    uint64_t token;
    bool cacheable;
} YanFsSource;

/* Memory context only. Its fields must not be written to disk as they are and
 * must not be edited by a caller to bypass the API. source_token and
 * source_cacheable are the in-memory source identity; they are not part of the
 * on-disk format and are never stored in a block. */
typedef struct {
    YanFsBlockIo io;
    bool initialized;
    YanFsState state;
    bool busy;
    uint32_t capacity_blocks;
    uint8_t metadata[4096];
    uint8_t scratch[4096];
    uint64_t source_token;
    bool source_cacheable;
} YanFs;

YanFsResult yan_fs_init(YanFs *, YanFsBlockIo);
YanFsResult yan_fs_mount(YanFs *);
YanFsResult yan_fs_unmount(YanFs *);
YanFsResult yan_fs_list(YanFs *, uint32_t *cursor, YanFsInfo *out);
YanFsResult yan_fs_stat(YanFs *, const char *name, YanFsInfo *out);
YanFsResult yan_fs_read(YanFs *, const char *name, uint32_t offset,
                       uint8_t *out, uint32_t length, uint32_t *read_bytes);
YanFsResult yan_fs_create(YanFs *, const char *name, const uint8_t *bytes, uint32_t length);
YanFsResult yan_fs_replace(YanFs *, const char *name, const uint8_t *bytes, uint32_t length);
YanFsResult yan_fs_remove(YanFs *, const char *name);

/* 0024 file management primitives. Both share the file-API state order
 * (context/initialized, BUSY, FAULTED, MOUNTED) and then validate both
 * caller-supplied names before they look anything up, so a missing source with
 * an invalid companion name is INVALID, not NOT_FOUND. The two name ranges are
 * read-only and may overlap each other (including sharing one string), but the
 * bytes actually read, terminator included, must not overlap the YanFs object.
 * yan_fs_rename rewrites the name field of the source's existing physical slot;
 * yan_fs_copy keeps the source slot and extent and allocates a new slot and a
 * new contiguous extent. */
YanFsResult yan_fs_rename(YanFs *, const char *old_name, const char *new_name);
YanFsResult yan_fs_copy(YanFs *, const char *source_name,
                       const char *destination_name);

/* 0026 pure observation: report the instance's current state and in-memory
 * source identity without reading the medium, allocating a token, changing the
 * metadata cache or reviving a faulted or unmounted source. The check order is
 * context + initialized, then BUSY, then the output holder's range, overflow
 * and context-alias check; an error writes nothing through out. A valid
 * UNMOUNTED, MOUNTED or FAULTED context all return YAN_FS_OK and report the
 * real state. */
YanFsResult yan_fs_source(const YanFs *fs, YanFsSource *out);

/* Pure encoding helpers: no I/O and no instance state. yan_fs_format_metadata
 * builds a canonical empty directory for a device of capacity_blocks and
 * computes its CRC; nothing is written unless the block is valid. The CRC is
 * the IEEE reflected CRC32 the format uses over the whole block with offsets
 * 60..63 treated as zero, which is where the CRC itself lives; reading the
 * block is the caller's precondition. Both are exported so a Host tool can
 * format an image; neither gives the Guest a way to format a device. */
YanFsResult yan_fs_format_metadata(uint8_t out[4096], uint32_t capacity_blocks);
uint32_t yan_fs_metadata_crc(const uint8_t block[4096]);

#endif
