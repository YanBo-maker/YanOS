#ifndef YAN_OS_NOTE_RELATIONS_H
#define YAN_OS_NOTE_RELATIONS_H

#include "note_metadata_format.h"
#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* 0028 knowledge-relations service: read-only navigation over the link and tag
 * declarations that 0027 stores in a note's header.
 *
 * The behavioural contract is docs/specs/0028-knowledge-relations.md,
 * "公共服务与接口责任". This header fixes the public seam that the later shell
 * binding and the native/oracle suites share:
 *
 *   - yan_relation_links: the declared outgoing links of one existing source,
 *     in declaration order, each target observed only through the directory.
 *   - yan_relation_backlinks: every existing file that declares one legal name,
 *     in ASCII filename order, after the whole directory head was prechecked.
 *   - yan_relation_related: existing files explained by a direct link in either
 *     direction or by a shared tag, sorted by (out+in) desc, shared tags desc,
 *     filename ASCII asc.
 *
 * Layering: the service borrows a caller-owned YanFs and depends on the pure
 * format os/note_metadata_format.h only. It includes no shell header, no Host
 * header and no test code, performs no Host I/O, allocates nothing and adds no
 * on-disk format. Every large object lives in the caller-owned
 * YanRelationContext, which must be zero-initialized before its first init and
 * held long-lived (static or otherwise off the 4 KiB task stack of 0019).
 * An idle context may be reinitialized; an active one returns BUSY.
 *
 * `links` and `related` require the named source to exist; `backlinks` treats
 * the name as a cited name that need not exist. None of the three writes to the
 * medium: a query is a pure observation of the current directory and headers,
 * and the caller keeps the borrowed filesystem stable for the whole call.
 *
 * The row is a borrowed, synchronous value: its name is valid only until the
 * row callback returns, and no pointer may be retained. The optional summary is
 * published only on YAN_NOTE_OK; every other result leaves it byte-identical. A
 * callback that returns false stops the walk with YAN_NOTE_STOPPED and publishes
 * no summary.
 *
 * Implementation status: yan_relation_init and the three read-only query entry
 * points are implemented. Each query runs the shared guard chain before any
 * device byte, holds the busy flag across the whole synchronous call, publishes
 * the optional summary only on YAN_NOTE_OK and is zero-write. The native suite
 * pins the observed contract; this comment describes the code and does not
 * claim a run result. */

/* Bounded header window and format caps shared with 0027. The service reads at
 * most YAN_RELATION_PREFIX_BYTES of any file, so a multi-MiB binary body is
 * never streamed. */
#define YAN_RELATION_PREFIX_BYTES UINT32_C(4096)
#define YAN_RELATION_MAX_SHARED_TAGS UINT32_C(16)
#define YAN_RELATION_RELATED_SHOWN UINT32_C(20)
#define YAN_RELATION_MAX_ROWS YAN_FS_MAX_FILES

/* One synchronised relation row. Every field is deterministic on every result
 * row: a field a command does not explain is always zero, never stale.
 *
 *   links     fills name and exists only; outgoing/incoming/shared_tags = 0
 *   backlinks fills name, exists = true and incoming = true; outgoing and
 *             shared_tags = 0 (the source of a backlink always exists)
 *   related   fills name, exists = true, outgoing, incoming and shared_tags
 *
 * shared_tags is an exact byte-identity count in 0..16; empty tag sets never
 * match, case is never folded and no Unicode normalisation is applied. */
typedef struct {
    char name[32];
    bool exists;
    bool outgoing;
    bool incoming;
    uint8_t shared_tags;
} YanRelationRow;

/* Result summary, published only on YAN_NOTE_OK.
 *
 *   links      total == shown, each <= 16, target_exists = true
 *   backlinks  total == shown, each <= 63, target_exists = the directory
 *              observation of the cited name (false is a normal dangling name)
 *   related    total <= 62 is every candidate, shown == min(total, 20),
 *              target_exists = true */
typedef struct {
    uint32_t total;
    uint32_t shown;
    bool target_exists;
} YanRelationSummary;

/* Synchronous row callback. `row` and its name are borrowed for this call only;
 * the caller copies anything it wants to keep. Returning false requests an
 * early stop, which the service reports as YAN_NOTE_STOPPED and publishes no
 * summary. A callback may query other objects, but re-entering the same
 * relation context is YAN_NOTE_BUSY with no additional I/O. */
typedef bool (*YanRelationRowFn)(void *context, const YanRelationRow *row);

/* Caller-owned, long-lived storage. A caller zero-initializes it before
 * yan_relation_init and never edits the fields afterwards. The bounded prefix,
 * parser workspace and source/candidate info are why this object is static;
 * the row table is borrowed from the caller and never goes on the stack. The
 * whole object stays inside the 64 KiB engineering budget. */
typedef struct {
    YanFs *fs;
    bool initialized;
    bool busy;

    /* Bounded header window for the source and for each observed candidate. */
    uint8_t prefix[YAN_RELATION_PREFIX_BYTES];
    /* Parser accumulator for one header at a time (never a stack object). */
    YanNoteParseWorkspace parse_workspace;
    /* Validated source metadata. */
    YanNoteInfo source_info;
    /* Validated metadata of the one candidate currently under observation. */
    YanNoteInfo candidate_info;
    /* 0036-R3: caller-owned relation/sort scratch, `row_capacity` rows, one per
     * file the image can hold. The context only borrows it. */
    YanRelationRow *rows;
    /* 0036-R8: caller-owned index scratch for the shared heapsort. */
    uint16_t *sort_index;
    uint32_t row_capacity;
    uint32_t row_count;
} YanRelationContext;

_Static_assert(sizeof(YanRelationContext) <= 65536u,
               "the relation context must stay inside the 64 KiB budget");

/* Records the borrowed filesystem and publishes an empty service. The accepted
 * order mirrors the other services: the context/borrowed-pointer spans and
 * their overlap, then BUSY, then the borrowed filesystem's state. `init` itself
 * must not touch the medium. A null context, a null or overlapping filesystem,
 * an uninitialized filesystem and a faulted filesystem are rejected without
 * writing the object; an initialized, idle context may be re-initialized after
 * validating the new pair, while an active context answers BUSY. */
YanNoteResult yan_relation_init(YanRelationContext *context, YanFs *fs,
                                YanRelationRow *rows, uint16_t *sort_index);
/* 0036-R3 storage size for one `capacity`-row table. */
uint32_t yan_relation_rows_storage_bytes(uint32_t capacity);
uint32_t yan_relation_sort_index_storage_bytes(uint32_t capacity);

/* Declares one existing source; emits one row per declared link in original
 * declaration order. A target is observed only through the directory, so a
 * missing target is exists=false (DANGLING) and a target with a broken header
 * is still exists=true. A missing source is YAN_NOTE_NOT_FOUND; a matched but
 * broken source header is YAN_NOTE_METADATA with no row and no summary. An old
 * header-less source succeeds with zero rows. */
YanNoteResult yan_relation_links(YanRelationContext *context, const char *name,
                                 YanRelationRowFn sink, void *sink_context,
                                 YanRelationSummary *summary);

/* Treats `name` as a cited name that need not exist. Every existing file is
 * prechecked over its bounded header first; one broken header is
 * YAN_NOTE_METADATA before any row is delivered. Each file that declares the
 * name exactly (case sensitive) yields one row in ASCII filename order, the
 * target's own self-declaration included. `summary->target_exists` reports the
 * directory observation of the cited name. */
YanNoteResult yan_relation_backlinks(YanRelationContext *context,
                                     const char *name, YanRelationRowFn sink,
                                     void *sink_context,
                                     YanRelationSummary *summary);

/* Requires the source to exist (a missing source is YAN_NOTE_NOT_FOUND before
 * any remote head is read). The whole directory is prechecked, candidates and
 * non-candidates alike, then existing files other than the source that have a
 * direct link in either direction or at least one shared tag are ranked by
 * (outgoing+incoming) descending, then shared_tags descending, then filename
 * ASCII ascending. `total` counts every candidate and `shown` caps the rows at
 * 20. */
YanNoteResult yan_relation_related(YanRelationContext *context, const char *name,
                                   YanRelationRowFn sink, void *sink_context,
                                   YanRelationSummary *summary);

#endif
