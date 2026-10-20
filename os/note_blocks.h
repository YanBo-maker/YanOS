#ifndef YAN_OS_NOTE_BLOCKS_H
#define YAN_OS_NOTE_BLOCKS_H

#include "note_blocks_format.h"
#include "note_metadata_format.h"
#include "yanfs.h"
#include <stdbool.h>
#include <stdint.h>

/* 0032 read-only navigation. All contexts are caller-owned and zeroed before
 * their first init, then held off the Guest's 4 KiB task stack. The filesystem
 * must remain stable for an entire call, including callbacks. No query writes
 * media or keeps a cache across calls. */
#define YAN_BLOCK_REFS_SHOWN UINT32_C(20)

typedef enum {
    YAN_BLOCK_TARGET_OK = 0,
    YAN_BLOCK_DANGLING_FILE,
    YAN_BLOCK_DANGLING_BLOCK
} YanBlockTargetState;

typedef struct {
    char source[32];
    /* list/show fill block; refs/backlinks fill reference. Other fields zero. */
    YanBlockDecl block;
    YanBlockReference reference;
    YanBlockTargetState state;
} YanBlockRow;

typedef struct {
    uint64_t total;
    uint32_t shown;
    uint32_t skipped;
    YanBlockTargetState target_state;
} YanBlockSummary;

/* Values and byte spans are borrowed only during the synchronous callback.
 * False stops with STOPPED and no summary publication. show calls the row sink
 * once to describe the range, then delivers its original bytes in chunks;
 * offset is relative to that range. A later read error remains an error even
 * when earlier chunks were already delivered. */
typedef bool (*YanBlockRowFn)(void *, const YanBlockRow *);
typedef bool (*YanBlockChunkFn)(void *, uint32_t offset,
                               const uint8_t *, uint32_t length);

typedef struct {
    YanFs *fs;
    bool initialized;
    bool busy;
    uint8_t buffer[4096];
    YanNoteParseWorkspace metadata_workspace;
    YanNoteInfo metadata;
    YanBlockTextValidator text;
    YanBlockParser parser;
    YanBlockRow rows[YAN_BLOCK_MAX_COUNT];
    /* 0036-R3: caller-owned, `name_capacity` names, one per directory slot. */
    char (*names)[32];
    /* 0036-R8: caller-owned index scratch for the shared heapsort. */
    uint16_t *sort_index;
    uint32_t name_capacity;
    uint32_t name_count;
    char current_file[32];
    YanBlockAddress filter;
    bool filtering;
    uint64_t total;
    uint32_t row_count;
    uint32_t skipped;
} YanBlockContext;

_Static_assert(sizeof(YanBlockContext) <= 65536u,
               "block navigation workspace exceeds the 0032 budget");

/* State/extent/BUSY checks precede other parameters and all I/O. Inputs and
 * typed output holders must not overlap the context or borrowed filesystem;
 * strings include their NUL, and inputs must not overlap the summary. The
 * optional summary is left byte-identical on every non-OK return. Opaque sink
 * objects have no declared extent: only their pointer byte can be checked.
 * Reentry, including init, is BUSY. init itself performs no I/O. */
YanBlockResult yan_blocks_init(YanBlockContext *, YanFs *, char (*names)[32],
                               uint16_t *sort_index);
/* 0036-R3 storage size for one `capacity`-name table. */
uint32_t yan_blocks_names_storage_bytes(uint32_t capacity);
uint32_t yan_blocks_sort_index_storage_bytes(uint32_t capacity);
YanBlockResult yan_blocks_list(YanBlockContext *, const char *name,
                               YanBlockRowFn, void *, YanBlockSummary *);
YanBlockResult yan_blocks_refs(YanBlockContext *, const char *name,
                               YanBlockRowFn, void *, YanBlockSummary *);
YanBlockResult yan_blocks_backlinks(YanBlockContext *, const YanBlockAddress *,
                                    YanBlockRowFn, void *, YanBlockSummary *);
YanBlockResult yan_blocks_show(YanBlockContext *, const YanBlockAddress *,
                               YanBlockRowFn, YanBlockChunkFn, void *,
                               YanBlockSummary *);
const char *yan_block_target_name(YanBlockTargetState);

#endif
