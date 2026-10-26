/* 0027 pure note-metadata format: native tests, written before the parser.
 *
 * The contract is os/note_metadata_format.h and docs/specs/0027-note-metadata.md
 * ("文件头及实际状态变化", "公共接口、状态与内存"). The suite was introduced
 * against RED stubs and now checks the implemented parser, encoder and field
 * validators. Compile/link failures are harness failures, not RED assertions.
 *
 * Byte facts this file pins, all measured from byte 0:
 *
 *   marker       `@yan-note 1\n` = 9 + 1 + 1 + 1 = 12 bytes
 *   close        `@end\n` = 5 bytes
 *   empty header 17 bytes, body offset 17
 *
 * header_bytes is the body offset: the byte after the closing LF, so it is 0
 * for a no-header file and 17 for the smallest legal header. The 4096-byte
 * whole-header cap counts marker, field lines and close together; a legal
 * header is bounded by 4096 bytes. The field caps restrict a fully populated
 * legal header to 2034 bytes; a longer offered API span is TOO_LARGE.
 *
 * What this file pins, clause by clause:
 *
 *   - recognition at byte 0 only: no prefix is an old no-header file, a
 *     partial prefix is still an old file, a BOM before the marker is an old
 *     file, a `@yan-note` prefix without the exact ` 1\n` marker (including
 *     the bare `@yan-note `) is METADATA, and a marker inside a body is not a
 *     header at all;
 *   - the smallest legal header, its body offset and the rule that only a
 *     fully valid header reports has_header; a no-header answer publishes an
 *     empty info;
 *   - a complete legal header with every field, its canonical order, tag/link
 *     order and body offset; the parser reads no body byte; a maximum-size
 *     legal header at exactly 2034 bytes, and a malformed 4096-byte prefix;
 *   - unknown version, unknown field, duplicate single value and duplicate or
 *     excess list entries as METADATA, never a guessed-empty header; a UTF-8
 *     BOM before a field key or inside a field value is METADATA, as confirmed
 *     by the SPEC owner on 2026-10-08;
 *   - strict LF and a mandatory closing marker, strict ASCII field spelling;
 *   - title 1..256 bytes with preserved spaces, invalid UTF-8 and every
 *     C0/DEL/C1 byte including TAB as INVALID_TEXT;
 *   - tag 1..63 bytes, 16 vs 17 distinct entries, the exact White_Space set,
 *     no case folding and no Unicode normalization;
 *   - link 1..31 byte YanFS names, 16 vs 17 distinct entries, "." and ".."
 *     refused;
 *   - Gregorian dates: leap 2000, non-leap 1900, year 0000/9999 and real
 *     month/day checks;
 *   - API protection: null pointers, workspace required and never aliased by
 *     the input, out/workspace overlap, bytes/out overlap, length_out/out and
 *     length_out/info overlap, length above the cap as TOO_LARGE and address
 *     overflow, plus the rule that an error leaves the caller's holders
 *     byte-identical.
 *
 * Mutation gates bind a suite, owner and exact assertion message. A message
 * in a table-driven loop identifies the checked rule, not an individual row.
 *
 * Every large object is file-scope static: a YanNoteInfo or a workspace is
 * about 2 KiB and a 4 KiB header fixture is larger still, so no test function
 * builds one on the stack. */

#include "note_metadata_format.h"
#include "note_metadata_format_internal.h"
#include "unity.h"

#include <string.h>

/* ------------------------------------------------------------- fixtures */

static YanNoteInfo info;
static YanNoteInfo parse_info;
static YanNoteParseWorkspace workspace;
static YanNoteParseWorkspace parse_workspace;

/* Recognition inputs. */
static const uint8_t old_body[] = "interrupt\n";                   /* no prefix */
static const uint8_t partial_prefix[] = "@yan-not";               /* too short */
static const uint8_t prefix_no_version[] = "@yan-note ";          /* prefix only */
static const uint8_t at_byte0[] = "@";                            /* one byte */
static const uint8_t empty_input[] = "";
static const uint8_t bom_prefix[] = "\xef\xbb\xbf@yan-note 1\n@end\n";
static const uint8_t bom_before_text[] = "\xef\xbb\xbfinterrupt\n";
static const uint8_t body_marker[] = "prose\n@yan-note 1\n@end\n";
static const uint8_t body_end_text[] = "@end\ninterrupt\n";
static const uint8_t empty_header[] = "@yan-note 1\n@end\n";      /* 17 bytes */
static const uint8_t header_only_lf[] = "@yan-note 1\n@end";      /* no final LF */
static const uint8_t header_inner_bom[] =
    "@yan-note 1\n\xef\xbb\xbftitle: a\n@end\n";

/* One complete legal header, split from its body so the expected body offset
 * is a measured length instead of a hand-counted number. After `title: ` the
 * delimiter consumes one space, so the value carries two leading spaces. */
static const uint8_t header_all_header[] =
    "@yan-note 1\n"
    "title:   \xe4\xb8\xad\xe6\x96\xad\xe7\xac\x94\xe8\xae\xb0\n"
    "created: 2026-10-07\n"
    "modified: 2026-12-31\n"
    "tag: os\n"
    "tag: riscv\n"
    "link: irq.txt\n"
    "link: missing.txt\n"
    "@end\n";
static const uint8_t header_all_body[] = "interrupt controller\n";
static const char header_all_title[] =
    "  \xe4\xb8\xad\xe6\x96\xad\xe7\xac\x94\xe8\xae\xb0";
static const uint8_t expected_all_header[] =
    "@yan-note 1\n"
    "title:   \xe4\xb8\xad\xe6\x96\xad\xe7\xac\x94\xe8\xae\xb0\n"
    "created: 2026-10-07\n"
    "modified: 2026-12-31\n"
    "tag: os\n"
    "tag: riscv\n"
    "link: irq.txt\n"
    "link: missing.txt\n"
    "@end\n";

static const uint8_t header_unknown_version[] =
    "@yan-note 2\n@end\n";
static const uint8_t header_unknown_field[] =
    "@yan-note 1\nkind: note\n@end\n";
static const uint8_t header_duplicate_title[] =
    "@yan-note 1\ntitle: a\ntitle: b\n@end\n";
static const uint8_t header_duplicate_created[] =
    "@yan-note 1\ncreated: 2026-10-07\ncreated: 2026-10-08\n@end\n";
static const uint8_t header_duplicate_tag_value[] =
    "@yan-note 1\ntag: os\ntag: os\n@end\n";
static const uint8_t header_duplicate_link_value[] =
    "@yan-note 1\nlink: a.txt\nlink: a.txt\n@end\n";
static const uint8_t header_missing_end[] =
    "@yan-note 1\ntitle: a\n";
static const uint8_t header_crlf[] =
    "@yan-note 1\r\ntitle: a\r\n@end\r\n";                        /* 23 bytes */
static const uint8_t header_no_space_after_colon[] =
    "@yan-note 1\ntitle:a\n@end\n";
static const uint8_t header_capital_key[] =
    "@yan-note 1\nTitle: a\n@end\n";
static const uint8_t error_header[] = "@yan-note 1\nkind: x\n@end\n";

/* Field-value fixtures. */
static const uint8_t title_ascii_1[] = "a";
static const uint8_t title_spaces[] = "  keep  ";
static const uint8_t title_tab[] = "a\tb";
static const uint8_t title_bad_c1[] = "a\xc2\x80z";   /* U+0080 as UTF-8 */
static const uint8_t title_bad_c0[] = "a\x01z";
static const uint8_t title_bad_del[] = "a\x7fz";
static const uint8_t title_bad_utf8[] = "a\xe4\xb8z"; /* truncated scalar */
static uint8_t title_256[256];
static uint8_t title_257[257];

static const uint8_t tag_ascii[] = "os";
static const uint8_t tag_case_a[] = "OS";
static const uint8_t tag_case_b[] = "os";
static const uint8_t tag_nfc[] = "caf\xc3\xa9";       /* U+00E9 */
static const uint8_t tag_nfd[] = "cafe\xcc\x81";      /* e + U+0301 */
static const uint8_t tag_zwsp[] = "\xe2\x80\x8b";     /* U+200B, not White_Space */
static const uint8_t tag_comma[] = "os,riscv";
static uint8_t tag_63[63];
static uint8_t tag_64[64];

/* Every member of the fixed White_Space set from the spec, one entry each. */
static const uint8_t whitespace_09[] = { 0x09 };
static const uint8_t whitespace_0a[] = { 0x0a };
static const uint8_t whitespace_0b[] = { 0x0b };
static const uint8_t whitespace_0c[] = { 0x0c };
static const uint8_t whitespace_0d[] = { 0x0d };
static const uint8_t whitespace_20[] = { 0x20 };
static const uint8_t whitespace_85[] = { 0xc2, 0x85 };
static const uint8_t whitespace_a0[] = { 0xc2, 0xa0 };
static const uint8_t whitespace_1680[] = { 0xe1, 0x9a, 0x80 };
static const uint8_t whitespace_2000[] = { 0xe2, 0x80, 0x80 };
static const uint8_t whitespace_2001[] = { 0xe2, 0x80, 0x81 };
static const uint8_t whitespace_2002[] = { 0xe2, 0x80, 0x82 };
static const uint8_t whitespace_2003[] = { 0xe2, 0x80, 0x83 };
static const uint8_t whitespace_2004[] = { 0xe2, 0x80, 0x84 };
static const uint8_t whitespace_2005[] = { 0xe2, 0x80, 0x85 };
static const uint8_t whitespace_2006[] = { 0xe2, 0x80, 0x86 };
static const uint8_t whitespace_2007[] = { 0xe2, 0x80, 0x87 };
static const uint8_t whitespace_2008[] = { 0xe2, 0x80, 0x88 };
static const uint8_t whitespace_2009[] = { 0xe2, 0x80, 0x89 };
static const uint8_t whitespace_200a[] = { 0xe2, 0x80, 0x8a };
static const uint8_t whitespace_2028[] = { 0xe2, 0x80, 0xa8 };
static const uint8_t whitespace_2029[] = { 0xe2, 0x80, 0xa9 };
static const uint8_t whitespace_202f[] = { 0xe2, 0x80, 0xaf };
static const uint8_t whitespace_205f[] = { 0xe2, 0x81, 0x9f };
static const uint8_t whitespace_3000[] = { 0xe3, 0x80, 0x80 };

typedef struct {
    const uint8_t *bytes;
    uint32_t length;
} WhitespaceCase;

static const WhitespaceCase whitespace_cases[] = {
    { whitespace_09, 1u },   { whitespace_0a, 1u },   { whitespace_0b, 1u },
    { whitespace_0c, 1u },   { whitespace_0d, 1u },   { whitespace_20, 1u },
    { whitespace_85, 2u },   { whitespace_a0, 2u },   { whitespace_1680, 3u },
    { whitespace_2000, 3u }, { whitespace_2001, 3u }, { whitespace_2002, 3u },
    { whitespace_2003, 3u }, { whitespace_2004, 3u }, { whitespace_2005, 3u },
    { whitespace_2006, 3u }, { whitespace_2007, 3u }, { whitespace_2008, 3u },
    { whitespace_2009, 3u }, { whitespace_200a, 3u }, { whitespace_2028, 3u },
    { whitespace_2029, 3u }, { whitespace_202f, 3u }, { whitespace_205f, 3u },
    { whitespace_3000, 3u }
};

static const uint8_t link_name_1[] = "a";
static const uint8_t link_name_31[] =
    "0123456789012345678901234567890";    /* 31 bytes */
static const uint8_t link_name_32[] =
    "01234567890123456789012345678901";   /* 32 bytes */
static const uint8_t link_dot[] = ".";
static const uint8_t link_dotdot[] = "..";
static const uint8_t link_slash[] = "dir/name";
static const uint8_t link_empty[] = "";

static const uint8_t date_ok[] = "2026-10-07";
static const uint8_t date_leap[] = "2000-02-29";
static const uint8_t date_nonleap[] = "1900-02-29";
static const uint8_t date_year0[] = "0000-01-01";
static const uint8_t date_year9999[] = "9999-12-31";
static const uint8_t date_bad_month[] = "2026-13-01";
static const uint8_t date_bad_day[] = "2026-04-31";
static const uint8_t date_short[] = "2026-10-0";
static const uint8_t date_long[] = "2026-10-071";
static const uint8_t date_slash[] = "2026/10/07";

/* Generated headers. Each builder writes marker + field lines + `@end\n` and
 * then one body byte so the body-offset rules stay observable. */
static uint8_t header_16_tags[4097];
static uint8_t header_17_tags[4097];
static uint8_t header_16_links[4097];
static uint8_t header_17_links[4097];
static uint8_t header_63_tag[4097];
static uint8_t header_64_tag[4097];
static uint8_t header_space_tag[4097];
static uint8_t header_title_256[4097];
static uint8_t header_title_257[4097];
static uint8_t header_max_legal[4096];
static uint8_t malformed_max_prefix[4097];
static uint8_t info_snapshot[sizeof(YanNoteInfo)];
static uint8_t field_value[64];

/* Alias fixtures. The cast storage is larger than the target struct and is
 * naturally aligned, and every alias below is rejected by pure address
 * arithmetic before a byte is read. */
_Alignas(YanNoteInfo) static uint8_t bytes_out_alias[128];
_Alignas(uint32_t) static uint8_t alias_out[128];
_Alignas(uint32_t) static uint8_t alias_len_storage[128];

/* Encoder outputs and holders. */
static uint8_t encode_out[4096];
static uint8_t encode_snapshot[64];
static uint8_t reparse_out[4096];
static uint8_t encode_small[8];
static uint32_t encode_length;
static uint32_t snapshot_length;

void setUp(void)
{
    memset(&info, 0, sizeof info);
    memset(&parse_info, 0, sizeof parse_info);
    memset(&workspace, 0, sizeof workspace);
    memset(&parse_workspace, 0, sizeof parse_workspace);
}

void tearDown(void)
{
}

/* --------------------------------------------------------------- helpers */

static void fill_repeated(uint8_t *out, size_t length, uint8_t first,
                          uint8_t repeated)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        out[i] = repeated;
    }
    if (length > 0u) {
        out[0] = first;
    }
}

static void append_bytes(uint8_t *out, size_t capacity, size_t *offset,
                         const uint8_t *bytes, size_t length)
{
    TEST_ASSERT_TRUE_MESSAGE(*offset + length <= capacity,
                             "test_append_room");
    memcpy(out + *offset, bytes, length);
    *offset += length;
}

static void append_literal(uint8_t *out, size_t capacity, size_t *offset,
                           const char *text)
{
    append_bytes(out, capacity, offset, (const uint8_t *)text, strlen(text));
}

static YanNoteResult parse_case(const uint8_t *bytes, uint32_t length)
{
    return yan_note_parse_header(bytes, length, &info, &workspace);
}

static void fill_valid_info(YanNoteInfo *target)
{
    static const uint8_t title[] = "note";
    static const uint8_t created[] = "2026-10-07";
    static const uint8_t tag[] = "os";
    static const uint8_t link[] = "irq.txt";

    memset(target, 0, sizeof *target);
    target->has_header = true;
    target->header_bytes = 64u;
    target->has_title = true;
    target->title_length = (uint16_t)(sizeof title - 1u);
    memcpy(target->title, title, sizeof title - 1u);
    target->has_created = true;
    target->created_length = (uint16_t)(sizeof created - 1u);
    memcpy(target->created, created, sizeof created - 1u);
    target->tag_count = 1u;
    target->tag_length[0] = (uint16_t)(sizeof tag - 1u);
    memcpy(target->tag[0], tag, sizeof tag - 1u);
    target->link_count = 1u;
    target->link_length[0] = (uint16_t)(sizeof link - 1u);
    memcpy(target->link[0], link, sizeof link - 1u);
    target->link_exists[0] = false;
}

/* A valid header with no declared field: the smallest legal header. */
static void fill_empty_info(YanNoteInfo *target)
{
    memset(target, 0, sizeof *target);
    target->has_header = true;
}

/* ------------------------------------------------- recognition at byte 0 */

static void note_parse_no_header_is_an_old_file(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(old_body, (uint32_t)(sizeof old_body - 1u)),
        "no_header_old_file_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header, "no_header_flag_false");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, info.header_bytes,
                                     "no_header_bytes_zero");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, info.tag_count,
                                     "no_header_no_tags");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, info.link_count,
                                     "no_header_no_links");

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(empty_input, 0u), "empty_input_old_file_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header, "empty_input_no_header_flag");
}

static void note_parse_partial_prefix_is_an_old_file(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(partial_prefix, (uint32_t)(sizeof partial_prefix - 1u)),
        "partial_prefix_yan_not_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header,
                              "partial_prefix_no_header_flag");

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(at_byte0, 1u), "single_at_byte_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header, "single_at_no_header_flag");
}

static void note_parse_prefix_without_version_is_metadata(void)
{
    /* `@yan-note ` already carries the whole 9-byte recognition prefix, so the
     * marker test runs and fails: this is METADATA, not an old file. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(prefix_no_version,
                   (uint32_t)(sizeof prefix_no_version - 1u)),
        "prefix_without_version_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(prefix_no_version, 9u),
        "nine_byte_prefix_metadata");
}

static void note_parse_bom_prefix_is_an_old_file(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(bom_prefix, (uint32_t)(sizeof bom_prefix - 1u)),
        "bom_then_marker_is_old_file_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header,
                              "bom_then_marker_no_header_flag");

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(bom_before_text, (uint32_t)(sizeof bom_before_text - 1u)),
        "bom_then_text_is_old_file_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header, "bom_then_text_no_header_flag");
}

static void note_parse_inner_bom_before_field_key_is_metadata(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_inner_bom,
                   (uint32_t)(sizeof header_inner_bom - 1u)),
        "inner_bom_before_key_metadata");
}

/* The owner confirmed the recognised-header FEFF ban includes field values.
 * These messages are the mutation gate's owner-specific assertion hooks. */
static void note_parse_feff_inside_a_value_is_metadata(void)
{
    static const uint8_t header_tag_feff[] =
        "@yan-note 1\ntag: a\xef\xbb\xbf\x62\n@end\n";
    static const uint8_t header_title_feff[] =
        "@yan-note 1\ntitle: a\xef\xbb\xbfz\n@end\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_tag_feff, (uint32_t)(sizeof header_tag_feff - 1u)),
        "feff_inside_tag_value_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_title_feff,
                   (uint32_t)(sizeof header_title_feff - 1u)),
        "feff_inside_title_value_metadata");
}

static void note_parse_exact_marker_without_close_is_metadata(void)
{
    static const uint8_t marker_only[] = "@yan-note 1\n";
    static const uint8_t marker_and_field[] = "@yan-note 1\ntitle: a\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(marker_only, 12u),
        "exact_marker_only_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(marker_and_field, (uint32_t)(sizeof marker_and_field - 1u)),
        "marker_and_field_without_close_metadata");
}

static void note_parse_marker_inside_body_is_not_a_header(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(body_marker, (uint32_t)(sizeof body_marker - 1u)),
        "marker_not_at_byte0_is_old_file_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header,
                              "marker_not_at_byte0_no_header_flag");

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(body_end_text, (uint32_t)(sizeof body_end_text - 1u)),
        "body_end_text_is_old_file_ok");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header,
                              "body_end_text_no_header_flag");
}

static void note_marker_constants_match_the_format(void)
{
    static const uint8_t expected_marker[12] = {
        (uint8_t)'@', (uint8_t)'y', (uint8_t)'a', (uint8_t)'n', (uint8_t)'-',
        (uint8_t)'n', (uint8_t)'o', (uint8_t)'t', (uint8_t)'e', (uint8_t)' ',
        (uint8_t)'1', (uint8_t)'\n'
    };
    static const uint8_t expected_prefix[9] = {
        (uint8_t)'@', (uint8_t)'y', (uint8_t)'a', (uint8_t)'n', (uint8_t)'-',
        (uint8_t)'n', (uint8_t)'o', (uint8_t)'t', (uint8_t)'e'
    };
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(12u, YAN_NOTE_MARKER_SIZE,
                                     "marker_size_is_twelve");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(17u, YAN_NOTE_EMPTY_HEADER_SIZE,
                                     "empty_header_size_is_seventeen");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_marker, yan_note_marker,
                                     sizeof expected_marker,
                                     "marker_bytes_exact");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_prefix, yan_note_marker_prefix,
                                     sizeof expected_prefix,
                                     "marker_prefix_bytes_exact");
}

/* ------------------------------------------------------- empty and legal */

static void note_parse_empty_header_reports_a_header(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(empty_header, (uint32_t)(sizeof empty_header - 1u)),
        "empty_header_ok");
    TEST_ASSERT_TRUE_MESSAGE(info.has_header, "empty_header_flag_true");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)(sizeof empty_header - 1u), info.header_bytes,
        "empty_header_bytes_is_marker_plus_end");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(17u, info.header_bytes,
                                     "empty_header_body_offset_is_seventeen");
    TEST_ASSERT_FALSE_MESSAGE(info.has_title, "empty_header_no_title");
    TEST_ASSERT_FALSE_MESSAGE(info.has_created, "empty_header_no_created");
    TEST_ASSERT_FALSE_MESSAGE(info.has_modified, "empty_header_no_modified");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, info.tag_count,
                                     "empty_header_no_tags");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, info.link_count,
                                     "empty_header_no_links");
}

static void note_parse_legal_maximum_header_and_bounded_prefix(void)
{
    size_t offset = 0u;
    for (size_t i = 0u; i < sizeof header_max_legal; ++i) {
        header_max_legal[i] = (uint8_t)' ';
    }
    append_literal(header_max_legal, sizeof header_max_legal, &offset,
                   "@yan-note 1\ntitle: ");
    append_bytes(header_max_legal, sizeof header_max_legal, &offset, title_256,
                 256u);
    append_literal(header_max_legal, sizeof header_max_legal, &offset, "\n");
    append_literal(header_max_legal, sizeof header_max_legal, &offset,
                   "created: 2026-10-07\n");
    append_literal(header_max_legal, sizeof header_max_legal, &offset,
                   "modified: 2026-12-31\n");
    for (uint32_t i = 0u; i < 16u; ++i) {
        fill_repeated(field_value, 63u, (uint8_t)('a' + (int)i),
                      (uint8_t)('a' + (int)i));
        append_literal(header_max_legal, sizeof header_max_legal, &offset,
                       "tag: ");
        append_bytes(header_max_legal, sizeof header_max_legal, &offset,
                     field_value, 63u);
        append_literal(header_max_legal, sizeof header_max_legal, &offset, "\n");
    }
    for (uint32_t i = 0u; i < 16u; ++i) {
        fill_repeated(field_value, 31u, (uint8_t)('a' + (int)i),
                      (uint8_t)('a' + (int)i));
        append_literal(header_max_legal, sizeof header_max_legal, &offset,
                       "link: ");
        append_bytes(header_max_legal, sizeof header_max_legal, &offset,
                     field_value, 31u);
        append_literal(header_max_legal, sizeof header_max_legal, &offset, "\n");
    }
    append_literal(header_max_legal, sizeof header_max_legal, &offset, "@end\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2034u, (uint32_t)offset,
                                     "header_max_fixture_is_2034");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(header_max_legal, (uint32_t)offset),
        "header_max_legal_at_2034_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2034u, info.header_bytes, "header_max_legal_body_offset_is_2034");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(16u, info.tag_count,
                                     "header_max_legal_tag_count");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(16u, info.link_count,
                                     "header_max_legal_link_count");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_TOO_LARGE, parse_case(malformed_max_prefix, 4097u),
        "header_offer_over_4096_is_too_large");
    memset(malformed_max_prefix, 'x', sizeof malformed_max_prefix);
    memcpy(malformed_max_prefix, "@yan-note 1\n", 12u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(malformed_max_prefix, 4096u),
        "header_unterminated_4096_is_metadata");
}

static void note_parse_all_fields_reports_values_and_body_offset(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(header_all_header,
                   (uint32_t)(sizeof header_all_header - 1u)),
        "all_fields_header_only_ok");
    TEST_ASSERT_TRUE_MESSAGE(info.has_header, "all_fields_header_flag");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)(sizeof header_all_header - 1u), info.header_bytes,
        "all_fields_body_offset");
    TEST_ASSERT_TRUE_MESSAGE(info.has_title, "all_fields_has_title");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(header_all_title, (const char *)info.title,
                                     "all_fields_title_keeps_two_spaces");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(
        (uint16_t)(sizeof header_all_title - 1u), info.title_length,
        "all_fields_title_length");
    TEST_ASSERT_TRUE_MESSAGE(info.has_created, "all_fields_has_created");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("2026-10-07",
                                     (const char *)info.created,
                                     "all_fields_created");
    TEST_ASSERT_TRUE_MESSAGE(info.has_modified, "all_fields_has_modified");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("2026-12-31",
                                     (const char *)info.modified,
                                     "all_fields_modified");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2u, info.tag_count, "all_fields_tag_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("os", (const char *)info.tag[0],
                                     "all_fields_tag_order_0");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("riscv", (const char *)info.tag[1],
                                     "all_fields_tag_order_1");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2u, info.link_count,
                                     "all_fields_link_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("irq.txt", (const char *)info.link[0],
                                     "all_fields_link_order_0");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("missing.txt", (const char *)info.link[1],
                                     "all_fields_link_order_1");
}

static void note_parse_never_reads_a_body_byte(void)
{
    static const uint8_t guarded[] =
        "@yan-note 1\ntitle: a\n@end\n\xff\x00\xc0\x80 huge binary body";
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(guarded, (uint32_t)(sizeof guarded - 1u)),
        "binary_body_after_end_ok");
    TEST_ASSERT_TRUE_MESSAGE(info.has_header,
                             "binary_body_after_end_header_flag");
}

static void note_encode_all_fields_roundtrips_canonically(void)
{
    size_t offset = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(header_all_header,
                   (uint32_t)(sizeof header_all_header - 1u)),
        "roundtrip_parse_ok");
    memset(encode_out, 0xa5, sizeof encode_out);
    encode_length = 0xdeadbeefu;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_encode_header(&info, encode_out, (uint32_t)sizeof encode_out,
                               &encode_length),
        "roundtrip_encode_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)(sizeof expected_all_header - 1u), encode_length,
        "roundtrip_length_matches_header_only");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_all_header, encode_out,
                                     sizeof expected_all_header - 1u,
                                     "roundtrip_bytes_canonical");

    append_bytes(reparse_out, sizeof reparse_out, &offset, encode_out,
                 encode_length);
    append_bytes(reparse_out, sizeof reparse_out, &offset, header_all_body,
                 sizeof header_all_body - 1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(reparse_out, (uint32_t)offset),
        "roundtrip_reparse_ok");
    TEST_ASSERT_TRUE_MESSAGE(info.has_header, "roundtrip_reparse_header_flag");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)(sizeof expected_all_header - 1u), info.header_bytes,
        "roundtrip_reparse_body_offset");
}

static void note_encode_keeps_list_order_and_canonical_field_order(void)
{
    static const uint8_t expected_empty[] = "@yan-note 1\n@end\n";
    static const uint8_t expected_title[] =
        "@yan-note 1\ntitle: x\ncreated: 2026-10-07\n@end\n";

    fill_empty_info(&info);
    memset(encode_out, 0xa5, sizeof encode_out);
    encode_length = 0xdeadbeefu;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_encode_header(&info, encode_out, (uint32_t)sizeof encode_out,
                               &encode_length),
        "empty_encode_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(17u, encode_length,
                                     "empty_encode_length_is_seventeen");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_empty, encode_out,
                                     sizeof expected_empty - 1u,
                                     "empty_encode_is_marker_plus_end");

    memset(&info, 0, sizeof info);
    info.has_header = true;
    info.has_title = true;
    info.title_length = 1u;
    info.title[0] = (uint8_t)'x';
    info.has_created = true;
    info.created_length = 10u;
    memcpy(info.created, "2026-10-07", 10u);
    memset(encode_out, 0xa5, sizeof encode_out);
    encode_length = 0xdeadbeefu;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_encode_header(&info, encode_out, (uint32_t)sizeof encode_out,
                               &encode_length),
        "title_created_encode_ok");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_title, encode_out,
                                     sizeof expected_title - 1u,
                                     "title_created_encode_canonical_order");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(sizeof expected_title - 1u),
                                     encode_length,
                                     "title_created_encode_length");
}

/* ------------------------------------------------ metadata error answers */

static void note_parse_unknown_version_is_metadata(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_unknown_version,
                   (uint32_t)(sizeof header_unknown_version - 1u)),
        "unknown_version_metadata");
}

static void note_parse_unknown_field_is_metadata(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_unknown_field,
                   (uint32_t)(sizeof header_unknown_field - 1u)),
        "unknown_field_metadata");
}

static void note_parse_duplicate_single_values_are_metadata(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_duplicate_title,
                   (uint32_t)(sizeof header_duplicate_title - 1u)),
        "duplicate_title_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_duplicate_created,
                   (uint32_t)(sizeof header_duplicate_created - 1u)),
        "duplicate_created_metadata");
}

static void note_parse_duplicate_list_entries_are_metadata(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_duplicate_tag_value,
                   (uint32_t)(sizeof header_duplicate_tag_value - 1u)),
        "duplicate_tag_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_duplicate_link_value,
                   (uint32_t)(sizeof header_duplicate_link_value - 1u)),
        "duplicate_link_metadata");
}

static void note_parse_missing_end_is_metadata(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_missing_end,
                   (uint32_t)(sizeof header_missing_end - 1u)),
        "missing_end_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_only_lf, (uint32_t)(sizeof header_only_lf - 1u)),
        "missing_end_lf_strict_metadata");
}

static void note_parse_strict_lf_only(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(header_crlf, 23u),
        "crlf_header_metadata");
}

static void note_parse_strict_field_spelling(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_no_space_after_colon,
                   (uint32_t)(sizeof header_no_space_after_colon - 1u)),
        "no_separator_space_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        parse_case(header_capital_key,
                   (uint32_t)(sizeof header_capital_key - 1u)),
        "capital_key_metadata");
}

/* -------------------------------------------------------- title validator */

static void note_title_validator_accepts_and_preserves(void)
{
    TEST_ASSERT_TRUE_MESSAGE(yan_note_title_valid(title_ascii_1, 1u),
                             "title_ascii_1_ok");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_title_valid(title_spaces, (uint32_t)(sizeof title_spaces - 1u)),
        "title_spaces_preserved");
    TEST_ASSERT_TRUE_MESSAGE(yan_note_title_valid(title_256, 256u),
                             "title_256_bytes_ok");
}

static void note_title_validator_rejects_empty_and_oversize(void)
{
    TEST_ASSERT_FALSE_MESSAGE(yan_note_title_valid(NULL, 0u),
                              "title_null_zero_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_title_valid(title_ascii_1, 0u),
                              "title_zero_length_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_title_valid(title_257, 257u),
                              "title_257_bytes_rejected");
}

static void note_title_validator_rejects_forbidden_bytes(void)
{
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_title_valid(title_tab, (uint32_t)(sizeof title_tab - 1u)),
        "title_tab_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_title_valid(title_bad_c0, (uint32_t)(sizeof title_bad_c0 - 1u)),
        "title_c0_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_title_valid(title_bad_del,
                             (uint32_t)(sizeof title_bad_del - 1u)),
        "title_del_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_title_valid(title_bad_c1, (uint32_t)(sizeof title_bad_c1 - 1u)),
        "title_c1_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_title_valid(title_bad_utf8,
                             (uint32_t)(sizeof title_bad_utf8 - 1u)),
        "title_invalid_utf8_rejected");
}

static void note_header_title_256_is_legal_and_257_is_metadata(void)
{
    size_t offset = 0u;
    append_literal(header_title_256, sizeof header_title_256, &offset,
                   "@yan-note 1\ntitle: ");
    append_bytes(header_title_256, sizeof header_title_256, &offset, title_256,
                 256u);
    append_literal(header_title_256, sizeof header_title_256, &offset,
                   "\n@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(header_title_256, (uint32_t)offset - 1u),
        "header_title_256_ok");

    offset = 0u;
    append_literal(header_title_257, sizeof header_title_257, &offset,
                   "@yan-note 1\ntitle: ");
    append_bytes(header_title_257, sizeof header_title_257, &offset, title_257,
                 257u);
    append_literal(header_title_257, sizeof header_title_257, &offset,
                   "\n@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(header_title_257, (uint32_t)offset - 1u),
        "header_title_257_metadata");
}

/* ---------------------------------------------------------- tag validator */

static void note_tag_validator_accepts_exact_bytes(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_tag_valid(tag_ascii, (uint32_t)(sizeof tag_ascii - 1u)),
        "tag_ascii_ok");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_tag_valid(tag_comma, (uint32_t)(sizeof tag_comma - 1u)),
        "tag_punctuation_ok");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_tag_valid(tag_zwsp, (uint32_t)(sizeof tag_zwsp - 1u)),
        "tag_unlisted_invisible_ok");
    TEST_ASSERT_TRUE_MESSAGE(yan_note_tag_valid(tag_63, 63u),
                             "tag_63_bytes_ok");
}

static void note_tag_validator_rejects_boundaries(void)
{
    TEST_ASSERT_FALSE_MESSAGE(yan_note_tag_valid(tag_64, 64u),
                              "tag_64_bytes_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_tag_valid(tag_ascii, 0u),
                              "tag_empty_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_tag_valid(NULL, 0u),
                              "tag_null_zero_rejected");
}

static void note_tag_validator_rejects_every_white_space(void)
{
    for (size_t i = 0u; i < sizeof whitespace_cases / sizeof whitespace_cases[0];
         ++i) {
        TEST_ASSERT_FALSE_MESSAGE(
            yan_note_tag_valid(whitespace_cases[i].bytes,
                               whitespace_cases[i].length),
            "tag_each_white_space_member_rejected");
    }
}

static void note_tag_identity_is_exact_not_folded_or_normalized(void)
{
    static const uint8_t header_upper[] = "@yan-note 1\ntag: OS\n@end\n";
    static const uint8_t header_nfd[] =
        "@yan-note 1\ntag: cafe\xcc\x81\n@end\n";
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_tag_valid(tag_case_a, (uint32_t)(sizeof tag_case_a - 1u)),
        "tag_upper_valid");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_tag_valid(tag_case_b, (uint32_t)(sizeof tag_case_b - 1u)),
        "tag_lower_valid");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_tag_valid(tag_nfc, (uint32_t)(sizeof tag_nfc - 1u)) &&
            yan_note_tag_valid(tag_nfd, (uint32_t)(sizeof tag_nfd - 1u)),
        "tag_nfc_and_nfd_both_valid");

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        parse_case(header_upper, (uint32_t)(sizeof header_upper - 1u)),
        "tag_upper_parse_ok");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("OS", (const char *)info.tag[0],
                                     "tag_case_is_not_folded");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(header_nfd, (uint32_t)(sizeof header_nfd - 1u)),
        "tag_nfd_parse_ok");
    TEST_ASSERT_EQUAL_STRING_MESSAGE((const char *)tag_nfd,
                                     (const char *)info.tag[0],
                                     "tag_normalization_is_not_applied");
}

/* --------------------------------------------------------- link validator */

static void note_name_validator_accepts_fs_names(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_name_valid(link_name_1, (uint32_t)(sizeof link_name_1 - 1u)),
        "name_one_byte_ok");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_name_valid(link_name_31, (uint32_t)(sizeof link_name_31 - 1u)),
        "name_31_bytes_ok");
}

static void note_name_validator_rejects_fs_name_boundaries(void)
{
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_name_valid(link_name_32, (uint32_t)(sizeof link_name_32 - 1u)),
        "name_32_bytes_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_name_valid(link_empty, 0u), "name_empty_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_name_valid(link_dot, 1u), "name_dot_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_name_valid(link_dotdot, 2u), "name_dotdot_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_name_valid(link_slash, (uint32_t)(sizeof link_slash - 1u)),
        "name_slash_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_name_valid(NULL, 0u),
                              "name_null_zero_rejected");
}

/* -------------------------------------------------------- date validator */

static void note_date_validator_accepts_real_dates(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_date_valid(date_ok, (uint32_t)(sizeof date_ok - 1u)),
        "date_2026_10_07_ok");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_date_valid(date_leap, (uint32_t)(sizeof date_leap - 1u)),
        "date_2000_02_29_leap_ok");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_note_date_valid(date_year9999,
                            (uint32_t)(sizeof date_year9999 - 1u)),
        "date_9999_12_31_ok");
}

static void note_date_validator_rejects_bad_gregorian(void)
{
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_nonleap,
                            (uint32_t)(sizeof date_nonleap - 1u)),
        "date_1900_02_29_nonleap_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_year0, (uint32_t)(sizeof date_year0 - 1u)),
        "date_0000_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_bad_month,
                            (uint32_t)(sizeof date_bad_month - 1u)),
        "date_month_13_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_bad_day,
                            (uint32_t)(sizeof date_bad_day - 1u)),
        "date_april_31_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_short, (uint32_t)(sizeof date_short - 1u)),
        "date_nine_bytes_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_long, (uint32_t)(sizeof date_long - 1u)),
        "date_eleven_bytes_rejected");
    TEST_ASSERT_FALSE_MESSAGE(
        yan_note_date_valid(date_slash, (uint32_t)(sizeof date_slash - 1u)),
        "date_slash_separator_rejected");
}

/* ------------------------------------------- generated list-count headers */

static void note_header_16_distinct_tags_ok_and_17_is_metadata(void)
{
    size_t offset = 0u;
    append_literal(header_16_tags, sizeof header_16_tags, &offset,
                   "@yan-note 1\n");
    for (uint32_t i = 0u; i < 16u; ++i) {
        fill_repeated(field_value, 2u, (uint8_t)('a' + (int)i),
                      (uint8_t)('a' + (int)i));
        append_literal(header_16_tags, sizeof header_16_tags, &offset, "tag: ");
        append_bytes(header_16_tags, sizeof header_16_tags, &offset,
                     field_value, 2u);
        append_literal(header_16_tags, sizeof header_16_tags, &offset, "\n");
    }
    append_literal(header_16_tags, sizeof header_16_tags, &offset, "@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(header_16_tags, (uint32_t)offset - 1u),
        "sixteen_distinct_tags_ok");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(16u, info.tag_count,
                                     "sixteen_distinct_tags_count");

    offset = 0u;
    append_literal(header_17_tags, sizeof header_17_tags, &offset,
                   "@yan-note 1\n");
    for (uint32_t i = 0u; i < 17u; ++i) {
        fill_repeated(field_value, 2u, (uint8_t)('a' + (int)i),
                      (uint8_t)('a' + (int)i));
        append_literal(header_17_tags, sizeof header_17_tags, &offset, "tag: ");
        append_bytes(header_17_tags, sizeof header_17_tags, &offset,
                     field_value, 2u);
        append_literal(header_17_tags, sizeof header_17_tags, &offset, "\n");
    }
    append_literal(header_17_tags, sizeof header_17_tags, &offset, "@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(header_17_tags, (uint32_t)offset - 1u),
        "seventeen_distinct_tags_metadata");
}

static void note_header_16_distinct_links_ok_and_17_is_metadata(void)
{
    size_t offset = 0u;
    append_literal(header_16_links, sizeof header_16_links, &offset,
                   "@yan-note 1\n");
    for (uint32_t i = 0u; i < 16u; ++i) {
        fill_repeated(field_value, 2u, (uint8_t)('a' + (int)i),
                      (uint8_t)('a' + (int)i));
        append_literal(header_16_links, sizeof header_16_links, &offset,
                       "link: ");
        append_bytes(header_16_links, sizeof header_16_links, &offset,
                     field_value, 2u);
        append_literal(header_16_links, sizeof header_16_links, &offset, "\n");
    }
    append_literal(header_16_links, sizeof header_16_links, &offset, "@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(header_16_links, (uint32_t)offset - 1u),
        "sixteen_distinct_links_ok");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(16u, info.link_count,
                                     "sixteen_distinct_links_count");

    offset = 0u;
    append_literal(header_17_links, sizeof header_17_links, &offset,
                   "@yan-note 1\n");
    for (uint32_t i = 0u; i < 17u; ++i) {
        fill_repeated(field_value, 2u, (uint8_t)('a' + (int)i),
                      (uint8_t)('a' + (int)i));
        append_literal(header_17_links, sizeof header_17_links, &offset,
                       "link: ");
        append_bytes(header_17_links, sizeof header_17_links, &offset,
                     field_value, 2u);
        append_literal(header_17_links, sizeof header_17_links, &offset, "\n");
    }
    append_literal(header_17_links, sizeof header_17_links, &offset, "@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(header_17_links, (uint32_t)offset - 1u),
        "seventeen_distinct_links_metadata");
}

static void note_header_tag_63_ok_and_64_is_metadata(void)
{
    size_t offset = 0u;
    append_literal(header_63_tag, sizeof header_63_tag, &offset,
                   "@yan-note 1\ntag: ");
    append_bytes(header_63_tag, sizeof header_63_tag, &offset, tag_63, 63u);
    append_literal(header_63_tag, sizeof header_63_tag, &offset, "\n@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, parse_case(header_63_tag, (uint32_t)offset - 1u),
        "header_tag_63_ok");

    offset = 0u;
    append_literal(header_64_tag, sizeof header_64_tag, &offset,
                   "@yan-note 1\ntag: ");
    append_bytes(header_64_tag, sizeof header_64_tag, &offset, tag_64, 64u);
    append_literal(header_64_tag, sizeof header_64_tag, &offset, "\n@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(header_64_tag, (uint32_t)offset - 1u),
        "header_tag_64_metadata");
}

static void note_header_white_space_tag_is_metadata(void)
{
    size_t offset = 0u;
    append_literal(header_space_tag, sizeof header_space_tag, &offset,
                   "@yan-note 1\ntag: ");
    append_bytes(header_space_tag, sizeof header_space_tag, &offset,
                 whitespace_20, 1u);
    append_literal(header_space_tag, sizeof header_space_tag, &offset,
                   "\n@end\nx");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA, parse_case(header_space_tag, (uint32_t)offset - 1u),
        "header_white_space_tag_metadata");
}

/* --------------------------------------------------- API protection rules */

static void note_test_invalid_arguments_protect_out(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header(NULL, 4u, &parse_info, &parse_workspace),
        "parse_null_bytes_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header(old_body, (uint32_t)(sizeof old_body - 1u), NULL,
                              &parse_workspace),
        "parse_null_out_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header(old_body, (uint32_t)(sizeof old_body - 1u),
                              &parse_info, NULL),
        "parse_null_workspace_invalid");
}

static void note_test_no_header_publishes_an_empty_info(void)
{
    YanNoteInfo *target = &parse_info;
    memset(target, 0xa5, sizeof *target);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_parse_header(old_body, (uint32_t)(sizeof old_body - 1u),
                              target, &parse_workspace),
        "no_header_publish_ok");
    TEST_ASSERT_FALSE_MESSAGE(target->has_header, "no_header_publishes_false");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, target->header_bytes,
                                     "no_header_publishes_zero_bytes");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, target->tag_count,
                                     "no_header_publishes_no_tags");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, target->link_count,
                                     "no_header_publishes_no_links");
    TEST_ASSERT_FALSE_MESSAGE(target->has_title, "no_header_publishes_no_title");
}

static void note_test_error_keeps_out_byte_identical(void)
{
    YanNoteInfo *target = &parse_info;
    memset(target, 0x5a, sizeof *target);
    memcpy(info_snapshot, target, sizeof info_snapshot);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        yan_note_parse_header(error_header,
                              (uint32_t)(sizeof error_header - 1u), target,
                              &parse_workspace),
        "error_metadata");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, target, sizeof info_snapshot,
                                     "error_keeps_out_full_snapshot");
}

static void note_test_input_length_is_bounded_not_read_past(void)
{
    static const uint8_t open_only[] = "@yan-note 1\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_parse_header(open_only, 0u, &parse_info, &parse_workspace),
        "zero_length_input_is_old_file");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        yan_note_parse_header(open_only, 9u, &parse_info, &parse_workspace),
        "nine_byte_magic_without_version_is_metadata");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        yan_note_parse_header(header_missing_end,
                              (uint32_t)(sizeof header_missing_end - 1u),
                              &parse_info, &parse_workspace),
        "full_marker_without_end_metadata");
}

static void note_test_input_and_workspace_alias_is_rejected(void)
{
    memset(&parse_workspace, 0, sizeof parse_workspace);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header((const uint8_t *)(void *)&parse_workspace,
                              (uint32_t)sizeof parse_workspace, &parse_info,
                              &parse_workspace),
        "parse_input_workspace_alias_invalid");
}

static void note_test_out_and_workspace_alias_is_rejected(void)
{
    memset(&parse_info, 0, sizeof parse_info);
    memset(&parse_workspace, 0, sizeof parse_workspace);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header(old_body, (uint32_t)(sizeof old_body - 1u),
                              (YanNoteInfo *)(void *)&parse_workspace,
                              &parse_workspace),
        "parse_out_inside_workspace_invalid");
}

static void note_test_bytes_out_overlap_is_rejected(void)
{
    memset(bytes_out_alias, 0x11, sizeof bytes_out_alias);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header(bytes_out_alias, (uint32_t)sizeof bytes_out_alias,
                              (YanNoteInfo *)(void *)bytes_out_alias,
                              &parse_workspace),
        "parse_bytes_out_overlap_invalid");
}

static void note_test_address_overflow_is_rejected(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header((const uint8_t *)(uintptr_t)UINTPTR_MAX, 4u,
                              &parse_info, &parse_workspace),
        "parse_bytes_span_overflow_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_parse_header(NULL, 0u, &parse_info,
            (YanNoteParseWorkspace *)(UINTPTR_MAX &
                ~((uintptr_t)_Alignof(YanNoteParseWorkspace) - 1u))),
        "parse_workspace_span_overflow_invalid");
}

static void note_test_encoder_rejects_invalid_arguments(void)
{
    fill_valid_info(&info);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(NULL, encode_out, (uint32_t)sizeof encode_out,
                               &encode_length),
        "encode_null_info_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(&info, NULL, (uint32_t)sizeof encode_out,
                               &encode_length),
        "encode_null_out_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(&info, encode_out, (uint32_t)sizeof encode_out,
                               NULL),
        "encode_null_length_out_invalid");
}

static void note_field_validators_reject_wrapping_inputs(void)
{
    const uint8_t *bad = (const uint8_t *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_FALSE_MESSAGE(yan_note_title_valid(bad, 1u),
                              "title_wrapping_span_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_tag_valid(bad, 1u),
                              "tag_wrapping_span_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_date_valid(bad, 10u),
                              "date_wrapping_span_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_name_valid(bad, 1u),
                              "name_wrapping_span_rejected");
}

static void note_encoder_rejects_absent_dates_with_nonzero_length(void)
{
    fill_valid_info(&info);
    info.has_created = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_note_encode_header(&info, encode_out, sizeof encode_out, &encode_length),
        "absent_created_with_length_rejected");
    fill_valid_info(&info);
    info.modified_length = 10u;
    memcpy(info.modified, "2026-10-07", 10u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_note_encode_header(&info, encode_out, sizeof encode_out, &encode_length),
        "absent_modified_with_length_rejected");
}

static void note_test_encoder_out_and_length_alias_is_rejected(void)
{
    fill_valid_info(&info);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(&info, alias_out, (uint32_t)sizeof alias_out,
                               (uint32_t *)(void *)alias_out),
        "encode_out_length_overlap_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(&info, alias_len_storage,
                               (uint32_t)sizeof alias_len_storage,
                               (uint32_t *)(void *)&info),
        "encode_length_out_info_overlap_invalid");
}

static void note_test_encode_rejects_incomplete_info_and_small_capacity(void)
{
    fill_valid_info(&info);
    info.has_title = false;
    info.title_length = 3u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(&info, encode_out, (uint32_t)sizeof encode_out,
                               &encode_length),
        "encode_half_set_title_invalid");

    fill_valid_info(&info);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_TOO_LARGE,
        yan_note_encode_header(&info, encode_small,
                               (uint32_t)sizeof encode_small, &encode_length),
        "encode_small_capacity_too_large");
}

static void note_test_encode_error_keeps_out_and_length_identical(void)
{
    memset(encode_out, 0x3c, sizeof encode_out);
    memcpy(encode_snapshot, encode_out, sizeof encode_snapshot);
    encode_length = 0x12345678u;
    snapshot_length = encode_length;

    fill_valid_info(&info);
    info.tag_count = 17u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_encode_header(&info, encode_out, (uint32_t)sizeof encode_out,
                               &encode_length),
        "encode_bad_tag_count_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(snapshot_length, encode_length,
                                     "encode_error_keeps_length");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(encode_snapshot, encode_out,
                                     sizeof encode_snapshot,
                                     "encode_error_keeps_out_prefix");
}

static void note_test_result_names_are_stable(void)
{
    TEST_ASSERT_EQUAL_STRING_MESSAGE("OK", yan_note_result_name(YAN_NOTE_OK),
                                     "result_name_ok");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("INVALID",
                                     yan_note_result_name(YAN_NOTE_INVALID),
                                     "result_name_invalid");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("METADATA",
                                     yan_note_result_name(YAN_NOTE_METADATA),
                                     "result_name_metadata");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("TOO_LARGE",
                                     yan_note_result_name(YAN_NOTE_TOO_LARGE),
                                     "result_name_too_large");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("INVALID_TEXT",
                                     yan_note_result_name(YAN_NOTE_INVALID_TEXT),
                                     "result_name_invalid_text");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("NOT_MOUNTED",
                                     yan_note_result_name(YAN_NOTE_NOT_MOUNTED),
                                     "result_name_not_mounted");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("FAULTED",
                                     yan_note_result_name(YAN_NOTE_FAULTED),
                                     "result_name_faulted");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("NOT_FOUND",
                                     yan_note_result_name(YAN_NOTE_NOT_FOUND),
                                     "result_name_not_found");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("NOSPACE",
                                     yan_note_result_name(YAN_NOTE_NOSPACE),
                                     "result_name_nospace");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("CORRUPT",
                                     yan_note_result_name(YAN_NOTE_CORRUPT),
                                     "result_name_corrupt");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "DIRECTORY_FULL",
        yan_note_result_name(YAN_NOTE_DIRECTORY_FULL),
        "result_name_directory_full");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("PROTOCOL",
                                     yan_note_result_name(YAN_NOTE_PROTOCOL),
                                     "result_name_protocol");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("UNSUPPORTED",
                                     yan_note_result_name(YAN_NOTE_UNSUPPORTED),
                                     "result_name_unsupported");
    TEST_ASSERT_NOT_NULL_MESSAGE(yan_note_result_name((YanNoteResult)99),
                                 "result_name_unknown_never_null");
}

static void note_encoder_accepts_exact_capacity(void)
{
    memset(&info, 0, sizeof info);
    encode_length = 99u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_encode_header(&info, encode_out, 17u, &encode_length),
        "encode_exact_17_capacity_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(17u, encode_length, "encode_exact_length");
    encode_length = 99u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_TOO_LARGE,
        yan_note_encode_header(&info, encode_out, 16u, &encode_length),
        "encode_one_short_capacity_rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(99u, encode_length, "encode_short_keeps_length");
}

static void note_title_validator_control_boundaries(void)
{
    const uint8_t c0[] = {0x1fu};
    const uint8_t space[] = {0x20u};
    const uint8_t c1[] = {0xc2u, 0x9fu};
    const uint8_t nbsp[] = {0xc2u, 0xa0u};
    TEST_ASSERT_FALSE_MESSAGE(yan_note_title_valid(c0, sizeof c0), "title_c0_last_rejected");
    TEST_ASSERT_TRUE_MESSAGE(yan_note_title_valid(space, sizeof space), "title_ascii_space_allowed");
    TEST_ASSERT_FALSE_MESSAGE(yan_note_title_valid(c1, sizeof c1), "title_c1_last_rejected");
    TEST_ASSERT_TRUE_MESSAGE(yan_note_title_valid(nbsp, sizeof nbsp), "title_nbsp_allowed");
}

static void note_body_offset_matches_full_parser_and_keeps_failures(void)
{
    const char *cases[] = {"", "prose", "@yan-note 1\n@end\nbody",
        "@yan-note 1\ntitle: \xe4\xb8\xad\nlink: f\ntag: t\n@end\nbody",
        "@yan-note 2\n@end\n", "@yan-note 1\ntag: x\ntag: x\n@end\n",
        "@yan-note 1\ntitle: \xef\xbb\xbf\n@end\n", "@yan-note 1\ntitle: \t\n@end\n"};
    for (uint32_t i=0u; i<sizeof cases/sizeof cases[0]; ++i) {
        uint32_t offset=UINT32_C(0xa5a5a5a5);
        uint32_t length=(uint32_t)strlen(cases[i]);
        YanNoteResult result=yan_note_parse_header((const uint8_t *)cases[i],length,&info,&workspace);
        TEST_ASSERT_EQUAL_MESSAGE(result,yan_note_body_offset((const uint8_t *)cases[i],length,&offset,&parse_workspace),"0040_offset_same_grammar");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(result==YAN_NOTE_OK ? info.header_bytes : UINT32_C(0xa5a5a5a5),offset,"0040_offset_atomic_publish");
    }
    uint32_t offset=99u;
    TEST_ASSERT_EQUAL(YAN_NOTE_OK,yan_note_body_offset(NULL,0u,&offset,&workspace));
    TEST_ASSERT_EQUAL_UINT32(0u,offset);
    offset=99u;
    TEST_ASSERT_EQUAL(YAN_NOTE_INVALID,yan_note_body_offset(NULL,1u,&offset,&workspace));
    TEST_ASSERT_EQUAL(YAN_NOTE_INVALID,yan_note_body_offset((const uint8_t *)&offset,sizeof offset,&offset,&workspace));
    TEST_ASSERT_EQUAL(YAN_NOTE_INVALID,yan_note_body_offset(old_body,sizeof old_body,(uint32_t *)(void *)&workspace,&workspace));
    TEST_ASSERT_EQUAL(YAN_NOTE_INVALID,yan_note_body_offset((const uint8_t *)&workspace,1u,&offset,&workspace));
    TEST_ASSERT_EQUAL(YAN_NOTE_INVALID,yan_note_body_offset((const uint8_t *)(UINTPTR_MAX-1u),3u,&offset,&workspace));
    TEST_ASSERT_EQUAL(YAN_NOTE_INVALID,yan_note_body_offset(old_body,sizeof old_body,(uint32_t *)(UINTPTR_MAX-1u),&workspace));
    TEST_ASSERT_EQUAL(YAN_NOTE_TOO_LARGE,yan_note_body_offset(old_body,4097u,&offset,&workspace));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(99u,offset,"0040_offset_bad_arguments_preserve_holder");
}


static void note_body_offset_reuses_dirty_workspace_without_public_pollution(void)
{
    static uint8_t many[4096];
    size_t used = 0u;
    append_literal(many, sizeof many, &used, "@yan-note 1\n");
    for (uint32_t i = 0u; i < 16u; ++i) {
        uint8_t value[] = {'t', (uint8_t)('a' + i)};
        append_literal(many, sizeof many, &used, "tag: ");
        append_bytes(many, sizeof many, &used, value, sizeof value);
        append_literal(many, sizeof many, &used, "\nlink: ");
        append_bytes(many, sizeof many, &used, value, sizeof value);
        append_literal(many, sizeof many, &used, "\n");
    }
    append_literal(many, sizeof many, &used,
                   "modified: 2026-10-26\ntitle: many\ncreated: 2026-01-01\n@end\n");
    const char *cases[] = {
        "@yan-note 1\nlink: z\ntag: z\ntitle: x\n@end\n",
        "@yan-note 1\ntitle: x\ncreated: 2026-01-01\ncreated: 2026-01-01\n@end\n",
        "@yan-note 1\nlink: z\nlink: z\n@end\n",
        "@yan-note 1\ntag: z\ntag: z\n@end\n",
        "@yan-note 1\n@end\n", "plain"
    };
    memset(&parse_workspace, 0xa5, sizeof parse_workspace);
    for (uint32_t pass = 0u; pass < 3u; ++pass) {
        for (uint32_t i = 0u; i <= sizeof cases / sizeof cases[0]; ++i) {
            const uint8_t *bytes = i == 0u ? many : (const uint8_t *)cases[i - 1u];
            uint32_t length = i == 0u ? (uint32_t)used : (uint32_t)strlen(cases[i - 1u]);
            uint32_t offset = UINT32_C(0xa5a5a5a5);
            YanNoteResult expected = yan_note_parse_header(bytes, length, &info, &workspace);
            TEST_ASSERT_EQUAL(expected, yan_note_body_offset(bytes, length, &offset, &parse_workspace));
            TEST_ASSERT_EQUAL_UINT32(expected == YAN_NOTE_OK ? info.header_bytes : UINT32_C(0xa5a5a5a5), offset);
            /* Passes 0/2 use only the internal parser continuously. Pass 1
             * also checks sparse-to-public publication, including all unused
             * slots and padding, against the separate full workspace. */
            if (pass == 1u) {
                TEST_ASSERT_EQUAL(expected, yan_note_parse_header(bytes, length,
                                                                 &parse_info, &parse_workspace));
                if (expected == YAN_NOTE_OK) {
                    TEST_ASSERT_EQUAL_MEMORY(&info, &parse_info, sizeof info);
                }
            }
        }
    }
}

int main(void)
{
    for (uint32_t i = 0u; i < sizeof title_256; ++i) {
        title_256[i] = (uint8_t)'a';
    }
    for (uint32_t i = 0u; i < sizeof title_257; ++i) {
        title_257[i] = (uint8_t)'a';
    }
    for (uint32_t i = 0u; i < sizeof tag_63; ++i) {
        tag_63[i] = (uint8_t)'t';
    }
    for (uint32_t i = 0u; i < sizeof tag_64; ++i) {
        tag_64[i] = (uint8_t)'t';
    }

    UNITY_BEGIN();
    RUN_TEST(note_body_offset_matches_full_parser_and_keeps_failures);
    RUN_TEST(note_body_offset_reuses_dirty_workspace_without_public_pollution);
    RUN_TEST(note_encoder_accepts_exact_capacity);
    RUN_TEST(note_title_validator_control_boundaries);
    RUN_TEST(note_parse_no_header_is_an_old_file);
    RUN_TEST(note_parse_partial_prefix_is_an_old_file);
    RUN_TEST(note_parse_prefix_without_version_is_metadata);
    RUN_TEST(note_parse_bom_prefix_is_an_old_file);
    RUN_TEST(note_parse_inner_bom_before_field_key_is_metadata);
    RUN_TEST(note_parse_feff_inside_a_value_is_metadata);
    RUN_TEST(note_parse_exact_marker_without_close_is_metadata);
    RUN_TEST(note_parse_marker_inside_body_is_not_a_header);
    RUN_TEST(note_marker_constants_match_the_format);
    RUN_TEST(note_parse_empty_header_reports_a_header);
    RUN_TEST(note_parse_legal_maximum_header_and_bounded_prefix);
    RUN_TEST(note_parse_all_fields_reports_values_and_body_offset);
    RUN_TEST(note_parse_never_reads_a_body_byte);
    RUN_TEST(note_encode_all_fields_roundtrips_canonically);
    RUN_TEST(note_encode_keeps_list_order_and_canonical_field_order);
    RUN_TEST(note_parse_unknown_version_is_metadata);
    RUN_TEST(note_parse_unknown_field_is_metadata);
    RUN_TEST(note_parse_duplicate_single_values_are_metadata);
    RUN_TEST(note_parse_duplicate_list_entries_are_metadata);
    RUN_TEST(note_parse_missing_end_is_metadata);
    RUN_TEST(note_parse_strict_lf_only);
    RUN_TEST(note_parse_strict_field_spelling);
    RUN_TEST(note_title_validator_accepts_and_preserves);
    RUN_TEST(note_title_validator_rejects_empty_and_oversize);
    RUN_TEST(note_title_validator_rejects_forbidden_bytes);
    RUN_TEST(note_header_title_256_is_legal_and_257_is_metadata);
    RUN_TEST(note_tag_validator_accepts_exact_bytes);
    RUN_TEST(note_tag_validator_rejects_boundaries);
    RUN_TEST(note_tag_validator_rejects_every_white_space);
    RUN_TEST(note_tag_identity_is_exact_not_folded_or_normalized);
    RUN_TEST(note_name_validator_accepts_fs_names);
    RUN_TEST(note_name_validator_rejects_fs_name_boundaries);
    RUN_TEST(note_date_validator_accepts_real_dates);
    RUN_TEST(note_date_validator_rejects_bad_gregorian);
    RUN_TEST(note_header_16_distinct_tags_ok_and_17_is_metadata);
    RUN_TEST(note_header_16_distinct_links_ok_and_17_is_metadata);
    RUN_TEST(note_header_tag_63_ok_and_64_is_metadata);
    RUN_TEST(note_header_white_space_tag_is_metadata);
    RUN_TEST(note_test_invalid_arguments_protect_out);
    RUN_TEST(note_test_no_header_publishes_an_empty_info);
    RUN_TEST(note_test_error_keeps_out_byte_identical);
    RUN_TEST(note_test_input_length_is_bounded_not_read_past);
    RUN_TEST(note_test_input_and_workspace_alias_is_rejected);
    RUN_TEST(note_test_out_and_workspace_alias_is_rejected);
    RUN_TEST(note_test_bytes_out_overlap_is_rejected);
    RUN_TEST(note_test_address_overflow_is_rejected);
    RUN_TEST(note_test_encoder_rejects_invalid_arguments);
    RUN_TEST(note_field_validators_reject_wrapping_inputs);
    RUN_TEST(note_encoder_rejects_absent_dates_with_nonzero_length);
    RUN_TEST(note_test_encoder_out_and_length_alias_is_rejected);
    RUN_TEST(note_test_encode_rejects_incomplete_info_and_small_capacity);
    RUN_TEST(note_test_encode_error_keeps_out_and_length_identical);
    RUN_TEST(note_test_result_names_are_stable);
    return UNITY_END();
}
