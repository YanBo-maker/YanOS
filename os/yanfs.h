#ifndef YAN_OS_YANFS_H
#define YAN_OS_YANFS_H

#include <stdbool.h>
#include <stdint.h>

/* YanFS core. The contract is docs/specs/0021-yanfs.md, and this header
 * repeats the constants, enums, structures and entry points fixed by its
 * "接口" section and nothing else.
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

/* Memory context only. Its fields must not be written to disk as they are and
 * must not be edited by a caller to bypass the API. */
typedef struct {
    YanFsBlockIo io;
    bool initialized;
    YanFsState state;
    bool busy;
    uint32_t capacity_blocks;
    uint8_t metadata[4096];
    uint8_t scratch[4096];
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
