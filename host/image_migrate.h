#ifndef YAN_HOST_IMAGE_MIGRATE_H
#define YAN_HOST_IMAGE_MIGRATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 0036-R7 offline image migration (Linux/WSL, C17). The flow, failure
 * ownership, `.assets` rules and verification are specified by
 * docs/specs/0036-yanfs-multi-block-directory.md R7; this header only fixes
 * the callable surface.
 *
 * Lifecycle: call once. OK means NEW and, when OLD.assets existed, NEW.assets
 * are fully written, fsynced, closed, remounted read-only and compared with
 * OLD. On failure only objects created by this call are removed; the 0035
 * <realpath(OLD)>.lock file is left in place. error receives a diagnostic on
 * failure and an empty string on OK; it may be NULL. The library never
 * prints, exits or writes OLD.
 *
 * Reused interfaces: yan_image_lock, yan_fs_format_directory /
 * yan_fs_directory_crc, yan_semantic_posix_open_image_ex / bind_source /
 * cleanup, and yan_semantic_source_digest. */
typedef enum {
    YAN_IMAGE_MIGRATE_OK = 0,
    YAN_IMAGE_MIGRATE_INVALID,
    YAN_IMAGE_MIGRATE_EXISTS,
    YAN_IMAGE_MIGRATE_BUSY,
    YAN_IMAGE_MIGRATE_CORRUPT,
    YAN_IMAGE_MIGRATE_UNSUPPORTED,
    YAN_IMAGE_MIGRATE_NOSPACE,
    YAN_IMAGE_MIGRATE_IO,
    YAN_IMAGE_MIGRATE_VERIFY
} YanImageMigrateResult;

typedef struct {
    /* N = 63..4095 and N = 64*D-1 for D=1..64. */
    uint32_t files;
    /* When false, B = OLD capacity_blocks + (D_new - D_old), v1 D_old = 1. */
    bool blocks_set;
    uint32_t blocks;
} YanImageMigrateOptions;

YanImageMigrateResult yan_image_migrate(const char *from, const char *to,
                                        const YanImageMigrateOptions *options,
                                        char *error, size_t error_size);

#endif
