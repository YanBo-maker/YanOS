#ifndef YAN_OS_NOTE_METADATA_H
#define YAN_OS_NOTE_METADATA_H

#include "note_metadata_format.h"
#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* 0027 note-metadata service: the filesystem-observing half of the feature.
 *
 * The pure byte format lives in os/note_metadata_format.h; this header borrows
 * a caller-owned YanFs and adds the three observable abilities from
 * docs/specs/0027-note-metadata.md, "命令及可观察输出" and "观察与更新的读取范围":
 *
 *   - observation: yan_note_show reads only the bounded header prefix of one
 *     file and reports the validated YanNoteInfo. The body may be multi-MiB
 *     binary and is never preflight-checked for an observation.
 *   - update: yan_note_set_title / yan_note_set_date / yan_note_tag /
 *     yan_note_link apply one declaration change. A no-change request (the
 *     same value, an existing tag/link add, a missing tag/link delete, or a
 *     clear of an absent field) succeeds with no body read and no write.
 *   - listing: yan_note_list reserves the whole directory first, then invokes
 *     one synchronous callback per eligible file.
 *
 * The service depends on YanFS and the pure format only. It includes no shell
 * header, no Host header and no test code, writes nothing to a device except
 * through yan_fs_replace, and is usable from the Guest. It never touches the
 * 4 KiB task stack with a large object: every buffer lives in the caller-owned
 * YanNoteContext, which must be static or otherwise long-lived.
 *
 * No on-disk format is added. A note is an ordinary YanFS file whose content
 * starts with the pure header; mv/cp carry it unchanged, and a header-less
 * file is an ordinary old note with empty metadata. The service invents no
 * yan_fs_meta_* entry point and no metadata block in the directory. */

/* The service's two work areas. The prefix area is the bounded header window
 * an observation and an update read; the candidate area holds one whole
 * resulting file (header + body) before the single yan_fs_replace. The spec's
 * rule is that the whole candidate is at most 16384 bytes including the
 * header. */
#define YAN_NOTE_PREFIX_BYTES UINT32_C(4096)
#define YAN_NOTE_CANDIDATE_BYTES UINT32_C(16384)

/* One directory snapshot entry: the borrowed name plus the last size the
 * directory reported. */
typedef struct {
    char name[32];
    uint32_t size_bytes;
} YanNoteEntry;

/* Which date field yan_note_set_date writes. No other value is legal. */
typedef enum {
    YAN_NOTE_DATE_CREATED = 0,
    YAN_NOTE_DATE_MODIFIED = 1
} YanNoteDateField;

/* What a tag/link request does. No other value is legal. */
typedef enum {
    YAN_NOTE_MANAGE_ADD = 0,
    YAN_NOTE_MANAGE_DELETE = 1
} YanNoteManageMode;

/* Synchronous listing callback. `name` is borrowed for this call only; the
 * caller copies a name it wants to keep. Returning false requests an early
 * stop, which the service reports as YAN_NOTE_STOPPED. */
typedef bool (*YanNoteListFn)(void *context, const char *name);

/* Caller-owned, long-lived storage. A caller zero-initializes it before
 * yan_note_init and never edits the fields afterwards. The bounded prefix,
 * candidate and parse workspace are why this object is static; the entry and
 * sort-name tables are borrowed from the caller and never go on the stack. */
typedef struct {
    YanFs *fs;
    bool initialized;
    bool busy;

    /* Bounded header window for observation and update preconditions. */
    uint8_t prefix[YAN_NOTE_PREFIX_BYTES];
    /* One whole resulting file, staged before the single yan_fs_replace. */
    uint8_t candidate[YAN_NOTE_CANDIDATE_BYTES];
    /* Current validated metadata and the pending candidate metadata. */
    YanNoteInfo current;
    YanNoteInfo working;
    /* Caller-provided workspace for the pure parser (never a stack object). */
    YanNoteParseWorkspace parse_workspace;
    /* Directory snapshot and the ASCII-ordered name table for yan_note_list.
     * 0036-R3: caller-owned, `capacity` entries, allocated once from the Guest
     * arena or the Host heap; the context only borrows them. */
    YanNoteEntry *entries;
    char (*sort_name)[32];
    uint32_t capacity;
    uint32_t entry_count;
} YanNoteContext;

_Static_assert(sizeof(YanNoteContext) <= 65536u,
               "the note context must stay inside the long-lived-object budget");

/* Records the borrowed filesystem and publishes an empty service. The accepted
 * order is the context/borrowed-pointer spans, then BUSY, then the borrowed
 * filesystem's state, then the argument rules. `init` itself must not touch the
 * medium. The lifecycle matches the other services: a null context, a null or
 * overlapping filesystem, an uninitialized filesystem for a fresh context, and
 * a faulted filesystem are rejected without writing the object; an initialized,
 * idle context may be re-initialized, which validates the new pair and resets
 * the service to empty, while an active context answers BUSY. */
YanNoteResult yan_note_init(YanNoteContext *context, YanFs *fs,
                            YanNoteEntry *entries, char (*sort_name)[32]);
/* 0036-R3 storage sizes for one `capacity`-entry directory. The Guest calls
 * these for its arena allocations and the Host for the matching calloc, so the
 * two sides cannot drift from sizeof. */
uint32_t yan_note_entries_storage_bytes(uint32_t capacity);
uint32_t yan_note_sort_name_storage_bytes(uint32_t capacity);

/* Reads one file's bounded header prefix and publishes the validated info.
 * `info_out` is written only on OK, so an error leaves it byte-identical. A
 * header-less file is a valid old note: empty metadata, not an error. A file
 * with a matched prefix and a broken header is YAN_NOTE_METADATA. A missing
 * file is YAN_NOTE_NOT_FOUND and is never created. The link_exists flags are
 * filled by a directory lookup of each declared link; a missing target is a
 * normal dangling link, not an error. */
YanNoteResult yan_note_show(YanNoteContext *context, const char *name,
                            YanNoteInfo *info_out);

/* Declares a title. `length == 0` clears the field. A request that would not
 * change the stored value returns OK with no body read and no write. An actual
 * change validates the whole resulting file, stages it in the candidate area
 * and publishes it with exactly one yan_fs_replace. An old header-less text
 * file gains the canonical header. */
YanNoteResult yan_note_set_title(YanNoteContext *context, const char *name,
                                 const uint8_t *bytes, uint32_t length);

/* Declares created or modified. A zero length clears the field; any other
 * length must be the strictly ten-byte Gregorian date. Same no-change and
 * publish rules as yan_note_set_title. */
YanNoteResult yan_note_set_date(YanNoteContext *context, const char *name,
                                YanNoteDateField field, const uint8_t *bytes,
                                uint32_t length);

/* Adds or deletes one tag. Identity is exact bytes with no folding. Adding an
 * existing tag and deleting a missing tag are successful no-ops. A change
 * keeps the original list order with the new entry appended. */
YanNoteResult yan_note_tag(YanNoteContext *context, const char *name,
                           YanNoteManageMode mode, const uint8_t *bytes,
                           uint32_t length);

/* Adds or deletes one link by target name. The name follows the YanFS rule.
 * Adding an existing link and deleting a missing link are successful no-ops. */
YanNoteResult yan_note_link(YanNoteContext *context, const char *name,
                            YanNoteManageMode mode, const char *target);

/* Lists eligible notes in ASCII name order. A NULL filter, a zero-count filter
 * and a filter whose every tag matches are equivalent; a filter's tags are
 * ANDed. count is the physical slot count (0..16); duplicate entries within
 * that bound are harmless. CLI tokens are deduplicated before storage. The whole directory is
 * checked first: if any file carries a broken header the call fails with
 * YAN_NOTE_METADATA and no callback runs and no count is published. Otherwise
 * one callback runs per matching file and *shown_out (when non-NULL) receives
 * the count of delivered names. A callback stop is YAN_NOTE_STOPPED with no
 * summary published. */
YanNoteResult yan_note_list(YanNoteContext *context,
                            const YanNoteTagFilter *filter,
                            YanNoteListFn callback, void *callback_context,
                            uint32_t *shown_out);

#endif
