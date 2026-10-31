#ifndef YAN_HOST_NOTE_WRITE_H
#define YAN_HOST_NOTE_WRITE_H

#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* Linux-only 0035 PR-A write adapter. operation is LOCK_EX or LOCK_SH. On
 * success *fd owns the lock; closing it releases the lock.
 *
 * Issue #53: the failure codes distinguish the causes that used to collapse
 * into a bare YAN_FS_IO, and yan_image_lock_errno additionally reports the
 * failing syscall's errno so a diagnostic can print strerror():
 *   YAN_FS_NOT_FOUND  realpath failed: the image is missing, or a path
 *                     component denies search; *lock_errno is ENOENT, EACCES,
 *                     ENOTDIR or ELOOP, whichever realpath set
 *   YAN_FS_IO         the ".lock" path could not be built (ENOMEM) or opened
 *                     (EACCES, EROFS, or ELOOP when O_NOFOLLOW refused a
 *                     symbolic link); *lock_errno names which
 *   YAN_FS_BUSY       another process holds the lock
 * YAN_FS_BUSY carries no errno: contention is not a syscall failure. Callers
 * should print strerror(*lock_errno) for the two failure codes rather than
 * re-deriving the cause from the code alone, because YAN_FS_IO covers more than
 * one errno. */
YanFsResult yan_image_lock(const char *image, int operation, int *fd);

/* Same lock, additionally reporting the failing syscall's errno through
 * *lock_errno (which may be NULL). *lock_errno is written only when the return
 * value is a failure code that has one, so a caller can print
 * strerror(*lock_errno) without re-deriving the cause. */
YanFsResult yan_image_lock_errno(const char *image, int operation, int *fd,
                                 int *lock_errno);

/* 0035-R7: YanFS name rule, 16384-byte editor limit and the 0032 text rule.
 * It never touches the filesystem. */
YanFsResult yan_note_write_check(const char *name, const uint8_t *text,
                                 uint32_t length);

/* 0035-R5/R6: check, lock, copy the image, create or replace on the copy,
 * fsync, rename and fsync the directory. *committed turns true only after the
 * rename, so a later directory fsync failure still reports it. */
YanFsResult yan_note_write(const char *image, bool replace, const char *name,
                           const uint8_t *text, uint32_t length,
                           bool *committed);

/* Same write body as yan_note_write without acquiring the lock. The caller
 * must already hold <realpath(image)>.lock exclusively. */
YanFsResult yan_note_write_locked(const char *image, bool replace,
                                  const char *name, const uint8_t *text,
                                  uint32_t length, bool *committed);

#endif
