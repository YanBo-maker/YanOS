#ifndef YAN_HOST_NOTE_WRITE_H
#define YAN_HOST_NOTE_WRITE_H

#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* Linux-only 0035 PR-A write adapter. operation is LOCK_EX or LOCK_SH. On
 * success *fd owns the lock; closing it releases the lock. */
YanFsResult yan_image_lock(const char *image, int operation, int *fd);

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

#endif
