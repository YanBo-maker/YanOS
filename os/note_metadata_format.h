#ifndef YAN_OS_NOTE_METADATA_FORMAT_H
#define YAN_OS_NOTE_METADATA_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure note-metadata format for 0027.
 *
 * The contract is docs/specs/0027-note-metadata.md, "文件头及实际状态变化",
 * "公共接口、状态与内存". This header is the shared format seam: it carries the
 * info/filter/result types plus the parser, encoder and field validators. It
 * must stay free of YanFS and of the shell, so the later term facade can reuse
 * YanNoteTagFilter without pulling in a filesystem, and so the same source
 * compiles for the Guest.
 *
 * The parser is implementation-complete for the locked 0027 behaviour and is
 * exercised tests-first: the suite in tests/test_note_metadata_format.c pins
 * every rule below. This comment describes the code, not a run result.
 *
 * Shape of a recognised header (syntax ASCII, values UTF-8, strict LF):
 *
 *   @yan-note 1\n            marker, the only legal one, 12 bytes
 *   title: VALUE\n           at most one, 1..256 UTF-8 bytes
 *   created: YYYY-MM-DD\n    at most one
 *   modified: YYYY-MM-DD\n   at most one
 *   tag: VALUE\n             1..63 UTF-8 bytes, at most 16, no White_Space
 *   link: NAME\n             YanFS name rules, at most 16
 *   @end\n                   closes the header, 5 bytes
 *
 * Recognition happens at byte 0 only. A file without the complete 9-byte
 * `@yan-note` prefix is an old no-header file: metadata is empty and no byte
 * is treated as header. Once the marker matches, a missing/mistyped close,
 * unknown field, unknown version or invalid field value is METADATA, never a
 * guessed-empty header. A UTF-8 BOM found inside a recognised header is
 * METADATA; a BOM that precedes the marker just makes the file a no-header
 * old file, because the marker must sit at byte 0.
 *
 * header_bytes is the body offset, measured from byte 0 of the file: the byte
 * after the closing LF. It is 0 for a no-header file and 17 for the smallest
 * legal header: the marker is 12 bytes (`@yan-note` 9 + space 1 + version 1 +
 * LF 1) and `@end\n` is 5, which is the spec's 17-byte empty header.
 *
 * Only the closing LF ends the header. There is no CRLF, no indentation, no
 * comment syntax and no BOM tolerance inside a recognised header. The parser
 * reads no body byte: it stops at the close or at 4096 bytes, whichever comes
 * first, so a multi-MiB binary body is never scanned for a metadata read. */

/* One note's declared metadata. A field that is absent carries a false
 * has_* flag and is never represented by an UNKNOWN sentinel value. */
typedef struct {
    /* True only when a complete legal header sits at byte 0. */
    bool has_header;
    /* Body offset from byte 0; see the header comment. 0 without a header. */
    uint32_t header_bytes;

    /* title */
    bool has_title;
    uint16_t title_length;
    uint8_t title[257];

    /* created / modified, strictly-ten-byte Gregorian YYYY-MM-DD */
    bool has_created;
    uint16_t created_length;
    uint8_t created[11];
    bool has_modified;
    uint16_t modified_length;
    uint8_t modified[11];

    /* tag list, kept in file order, exact bytes, never folded */
    uint16_t tag_count;
    uint16_t tag_length[16];
    uint8_t tag[16][64];

    /* link list, kept in file order; each name is a 32-byte NUL-terminated
     * YanFS name. link_exists is an observation filled by the service layer,
     * not by the parser. */
    uint16_t link_count;
    uint16_t link_length[16];
    uint8_t link[16][32];
    bool link_exists[16];
} YanNoteInfo;

/* AND selection with 16 physical slots: count must be 0..16 before any slot
 * is read. Duplicate entries within that bound are harmless. CLI parsers
 * deduplicate raw tokens before storing them, so repeated tokens do not consume
 * the limit of 16 distinct tags. Identity uses the exact 63-byte tag rule. */
typedef struct {
    uint16_t count;
    uint16_t length[16];
    uint8_t tag[16][64];
} YanNoteTagFilter;

/* Note-layer outcome. OK/INVALID/BUSY/STOPPED are lifecycle; METADATA is an
 * ordinary content error (a broken header, not a dead filesystem);
 * TOO_LARGE and INVALID_TEXT belong to the update path; the remaining names
 * mirror the underlying YanFS errors (including CORRUPT and DIRECTORY_FULL) so
 * the service can report them without collapsing METADATA into a filesystem
 * failure. */
typedef enum {
    YAN_NOTE_OK = 0,
    YAN_NOTE_INVALID = 1,
    YAN_NOTE_BUSY = 2,
    YAN_NOTE_STOPPED = 3,
    YAN_NOTE_METADATA = 4,
    YAN_NOTE_TOO_LARGE = 5,
    YAN_NOTE_INVALID_TEXT = 6,
    YAN_NOTE_NOT_MOUNTED = 7,
    YAN_NOTE_FAULTED = 8,
    YAN_NOTE_NOT_FOUND = 9,
    YAN_NOTE_EXISTS = 10,
    YAN_NOTE_NOSPACE = 11,
    YAN_NOTE_CORRUPT = 12,
    YAN_NOTE_DIRECTORY_FULL = 13,
    YAN_NOTE_IO = 14,
    YAN_NOTE_PROTOCOL = 15,
    YAN_NOTE_UNSUPPORTED = 16
} YanNoteResult;

/* The only legal marker, `@yan-note 1\n` (12 bytes: 9-byte prefix, space,
 * version digit, LF) and its 9-byte recognition prefix, `@yan-note`. They are
 * exported so tests, the service and the oracle probe share one constant
 * instead of respelling the bytes. */
extern const uint8_t yan_note_marker[12];
extern const uint8_t yan_note_marker_prefix[9];

/* Fixed format caps from the spec. */
#define YAN_NOTE_MARKER_SIZE UINT32_C(12)
#define YAN_NOTE_PREFIX_SIZE UINT32_C(9)
#define YAN_NOTE_END_SIZE UINT32_C(5)
#define YAN_NOTE_EMPTY_HEADER_SIZE UINT32_C(17)
#define YAN_NOTE_HEADER_MAX UINT32_C(4096)
#define YAN_NOTE_TITLE_MAX UINT32_C(256)
#define YAN_NOTE_TAG_MAX_COUNT UINT32_C(16)
#define YAN_NOTE_TAG_MAX UINT32_C(63)
#define YAN_NOTE_LINK_MAX_COUNT UINT32_C(16)
#define YAN_NOTE_LINK_NAME_MAX UINT32_C(31)
#define YAN_NOTE_DATE_LENGTH UINT32_C(10)

/* Caller-owned parse workspace. The parser keeps its candidate info here and
 * copies it out only after the whole header validated, so a ~2 KiB
 * accumulation never lands on the 4 KiB task stack of 0019; the object is
 * static or otherwise long-lived at every call site. The workspace holds the
 * accumulator, never a pointer into the input. It
 * must not overlap the caller's info or the offered input span. */
typedef struct {
    YanNoteInfo info;
} YanNoteParseWorkspace;

_Static_assert(sizeof(YanNoteParseWorkspace) <= 4096u,
               "the parse workspace must stay inside the small-object budget");

/* Field validators. They are pure, allocate nothing and read only the declared
 * range. The parser and encoder share them, and the tests use them to pin the
 * field rules directly, so the later mutation owners can bind one assertion
 * per rule.
 *
 *   yan_note_title_valid 1..256 UTF-8 bytes, single line, no C0/DEL/C1 and
 *                        no TAB; leading/trailing spaces are preserved
 *   yan_note_tag_valid   1..63 UTF-8 bytes, no control and no member of the
 *                        fixed Unicode White_Space set, exact bytes
 *   yan_note_date_valid  exactly YYYY-MM-DD, Gregorian, year 0001..9999 with
 *                        real month/leap/day
 *   yan_note_name_valid  the YanFS 1..31 byte ASCII name rule, case
 *                        sensitive, "." and ".." refused
 */
bool yan_note_title_valid(const uint8_t *bytes, uint32_t length);
bool yan_note_tag_valid(const uint8_t *bytes, uint32_t length);
bool yan_note_date_valid(const uint8_t *bytes, uint32_t length);
bool yan_note_name_valid(const uint8_t *bytes, uint32_t length);

/* Parses up to `length` prefix bytes of one file and publishes *out only on
 * OK. `bytes` may be NULL only when length is 0. The parser reads at most
 * YAN_NOTE_HEADER_MAX bytes and never a body byte past the close.
 *
 * OK means the classification is complete and decisive: either a fully valid
 * header (has_header true, header_bytes = body offset), or a file that cannot
 * carry a header (has_header false, header_bytes 0). METADATA means the
 * `@yan-note` prefix was matched at byte 0 and the header is broken; it is a
 * content error, not an I/O error. A length above YAN_NOTE_HEADER_MAX is
 * TOO_LARGE.
 *
 * Aliasing and range rules: workspace and out are both required, may not
 * overlap each other, and their spans may not leave uintptr_t; bytes may not
 * overlap out or workspace. An error leaves *out byte-identical, and only OK
 * writes it. No-header success publishes empty metadata. */
YanNoteResult yan_note_parse_header(const uint8_t *bytes, uint32_t length,
                                    YanNoteInfo *out,
                                    YanNoteParseWorkspace *workspace);

/* Encodes the canonical header for one fully valid info and writes only the
 * header, never a body. Canonical order is title, created, modified, then the
 * tags in original order, then the links in original order; absent fields are
 * omitted. `capacity` counts writable bytes at out.
 *
 * On OK *length_out receives the body offset (the same value the parser would
 * report), and out[0..*length_out) holds the header. An info whose has_title/
 * has_created/has_modified is false must carry length 0 for that field and may
 * not carry a half-set value; the validators run over every present field
 * before any byte is written.
 *
 * Aliasing and range rules: out and length_out are both required and distinct;
 * their spans may not overlap each other and may not leave uintptr_t; out may
 * not overlap the info; length_out also may not overlap info. TOO_LARGE means
 * the canonical header cannot fit the
 * capacity or the 4096-byte format cap. Any error leaves out and *length_out
 * byte-identical: a failed encode never publishes a partial or padded buffer. */
YanNoteResult yan_note_encode_header(const YanNoteInfo *info, uint8_t *out,
                                     uint32_t capacity, uint32_t *length_out);

/* Stable ASCII token for one result, for diagnostics and for tests that want a
 * readable failure message. Never returns NULL. */
const char *yan_note_result_name(YanNoteResult result);

#endif
