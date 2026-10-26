#include "note_blocks_format.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static YanBlockParser parser;
static YanBlockTextValidator validator;
static YanBlockReference rows[400];
static uint64_t row_count;
static uint8_t large[110000];
static bool refuse_row;
static bool reenter;
static YanBlockResult callback_init, callback_feed, callback_finish;

void setUp(void)
{
    memset(&parser, 0, sizeof parser);
    memset(&validator, 0, sizeof validator);
    memset(rows, 0, sizeof rows);
    row_count = 0u;
    refuse_row = false;
    reenter = false;
    callback_init = callback_feed = callback_finish = YAN_BLOCK_OK;
}

void tearDown(void) {}

static bool collect(void *context, const YanBlockReference *reference)
{
    TEST_ASSERT_EQUAL_PTR_MESSAGE(&row_count, context, "format_callback_context_exact");
    if (row_count < sizeof rows / sizeof rows[0]) {
        rows[row_count] = *reference;
    }
    ++row_count;
    if (reenter) {
        callback_init = yan_block_parser_init(&parser, 0u, 1u, NULL, NULL);
        callback_feed = yan_block_parser_feed(&parser, NULL, 0u);
        callback_finish = yan_block_parser_finish(&parser);
    }
    return !refuse_row;
}

static YanBlockResult scan_at(const uint8_t *bytes, uint32_t length,
                              uint32_t chunk, uint32_t offset,
                              uint32_t first_line)
{
    YanBlockResult result = yan_block_parser_init(&parser, offset, first_line,
                                                 collect, &row_count);
    for (uint32_t done = 0u; result == YAN_BLOCK_OK && done < length;) {
        uint32_t amount = length - done;
        if (amount > chunk) {
            amount = chunk;
        }
        result = yan_block_parser_feed(&parser, bytes + done, amount);
        done += amount;
    }
    return result == YAN_BLOCK_OK ? yan_block_parser_finish(&parser) : result;
}

static YanBlockResult scan(const char *text, uint32_t chunk)
{
    return scan_at((const uint8_t *)text, (uint32_t)strlen(text), chunk, 0u, 1u);
}

static YanBlockResult text_check(const uint8_t *bytes, uint32_t length,
                                 uint32_t chunk)
{
    YanBlockResult result = yan_block_text_init(&validator);
    for (uint32_t done = 0u; result == YAN_BLOCK_OK && done < length;) {
        uint32_t amount = length - done;
        if (amount > chunk) {
            amount = chunk;
        }
        result = yan_block_text_feed(&validator, bytes + done, amount);
        done += amount;
    }
    return result == YAN_BLOCK_OK ? yan_block_text_finish(&validator) : result;
}

static void format_id_rules(void)
{
    uint8_t id[32];
    memset(id, 'a', sizeof id);
    TEST_ASSERT_TRUE_MESSAGE(yan_block_id_valid((const uint8_t *)"A_z-19", 6u), "format_id_ascii_exact");
    TEST_ASSERT_TRUE_MESSAGE(yan_block_id_valid(id, 31u), "format_id_31_allowed");
    TEST_ASSERT_FALSE_MESSAGE(yan_block_id_valid(id, 32u), "format_id_32_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_block_id_valid(id, 0u), "format_id_empty_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_block_id_valid((const uint8_t *)"a.b", 3u), "format_id_dot_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_block_id_valid((const uint8_t *)"a b", 3u), "format_id_space_rejected");
    TEST_ASSERT_FALSE_MESSAGE(yan_block_id_valid((const uint8_t *)"\xc3\xa9", 2u), "format_id_unicode_rejected");
}

static void format_address_rules_and_atomic_out(void)
{
    YanBlockAddress address, before;
    memset(&address, 0xa5, sizeof address);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, yan_block_address_parse((const uint8_t *)"A-1.txt#Block_1", 15u, &address), "format_address_valid");
    TEST_ASSERT_EQUAL_STRING("A-1.txt", address.name);
    TEST_ASSERT_EQUAL_STRING("Block_1", address.id);
    const char *invalid[] = {"a", "a#", "#b", ".#b", "..#b", "a/b#c", "a#b#c", "a#b.c", "a#b c"};
    for (uint32_t i = 0u; i < sizeof invalid / sizeof invalid[0]; ++i) {
        before = address;
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID, yan_block_address_parse((const uint8_t *)invalid[i], (uint32_t)strlen(invalid[i]), &address), "format_address_invalid");
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &address, sizeof address, "format_address_error_preserves_out");
    }
    uint8_t max_address[63];
    memset(max_address, 'a', sizeof max_address);
    max_address[31] = '#';
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, yan_block_address_parse(max_address, sizeof max_address, &address), "format_address_31_each_allowed");
}

static void format_text_valid_chunks_and_feff(void)
{
    const uint8_t legal[] = {'\t', '\n', '\r', '\n', 'A', 0xc2u, 0xa0u,
                             0xe4u, 0xb8u, 0xadu, 0xefu, 0xbbu, 0xbfu,
                             0xf4u, 0x8fu, 0xbfu, 0xbfu};
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, text_check(NULL, 0u, 1u), "format_text_empty_valid");
    for (uint32_t chunk = 1u; chunk <= sizeof legal; ++chunk) {
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, text_check(legal, sizeof legal, chunk), "format_text_utf8_crlf_feff_valid");
    }
}

static void format_text_controls_and_bare_cr(void)
{
    for (uint32_t value = 0u; value <= 0x7fu; ++value) {
        uint8_t byte = (uint8_t)value;
        if ((value < 0x20u && value != '\t' && value != '\n') || value == 0x7fu) {
            TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check(&byte, 1u, 1u), "format_text_c0_del_rejected");
        }
    }
    for (uint32_t value = 0x80u; value <= 0x9fu; ++value) {
        uint8_t bytes[2] = {0xc2u, (uint8_t)value};
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check(bytes, 2u, 1u), "format_text_c1_rejected");
    }
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check((const uint8_t *)"a\rb", 3u, 1u), "format_text_bare_cr_rejected");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check((const uint8_t *)"\r\r\n", 3u, 1u), "format_text_double_cr_rejected");
}

static void format_text_invalid_utf8_and_truncation(void)
{
    const uint8_t invalid[][4] = {{0x80u}, {0xc0u, 0x80u}, {0xe0u, 0x80u, 0x80u},
        {0xedu, 0xa0u, 0x80u}, {0xf4u, 0x90u, 0x80u, 0x80u},
        {0xf5u, 0x80u, 0x80u, 0x80u}, {0xe4u, 'a', 0x80u}};
    const uint32_t lengths[] = {1u, 2u, 3u, 3u, 4u, 4u, 3u};
    for (uint32_t i = 0u; i < sizeof lengths / sizeof lengths[0]; ++i) {
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check(invalid[i], lengths[i], 1u), "format_text_invalid_utf8_rejected");
    }
    const uint8_t scalar[] = {0xf0u, 0x9fu, 0x98u, 0x80u};
    for (uint32_t length = 1u; length < sizeof scalar; ++length) {
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check(scalar, length, 1u), "format_text_truncation_rejected_at_finish");
    }
}

static void format_text_ascii_run_preserves_transition_state_and_bounds(void)
{
    const uint8_t prefix[] = {'A', '\t', '~', '\n', 'B', '\r', 0u};
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_init(&validator));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_feed(&validator, prefix, 6u));
    TEST_ASSERT_TRUE(validator.pending_cr);
    TEST_ASSERT_EQUAL_UINT8(0u, validator.utf8_length);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_feed(&validator, (const uint8_t *)"\nabc", 4u));
    TEST_ASSERT_FALSE(validator.pending_cr);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_finish(&validator));
    const uint8_t split[] = {'x', 'y', 0xe4u, 0xb8u, 0xadu, 'z', 0u};
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_init(&validator));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_feed(&validator, split, 3u));
    TEST_ASSERT_EQUAL_UINT8(1u, validator.utf8_length);
    TEST_ASSERT_FALSE(validator.pending_cr);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_feed(&validator, split + 3u, 3u));
    TEST_ASSERT_EQUAL_UINT8(0u, validator.utf8_length);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_finish(&validator));
    for (uint32_t value = 0u; value <= 0x7fu; ++value) {
        if ((value < 0x20u && value != '\t' && value != '\n') || value == 0x7fu) {
            uint8_t bytes[] = {'a', 'b', (uint8_t)value, 'c'};
            TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID_TEXT, text_check(bytes, sizeof bytes, sizeof bytes));
        }
    }
    const uint8_t c1[] = {'a', 'b', 0xc2u, 0x80u, 'c'};
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID_TEXT, text_check(c1, sizeof c1, sizeof c1));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, text_check(split, 6u, 6u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID_TEXT, text_check(split, 7u, 7u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID_TEXT, text_check(split, 3u, 3u));
}

static void format_text_late_damage_and_long_input(void)
{
    memset(large, 'x', sizeof large);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, text_check(large, sizeof large, 4096u), "format_text_over_16k_valid");
    large[sizeof large - 1u] = 0u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID_TEXT, text_check(large, sizeof large, 4096u), "format_text_final_nul_rejected");
}

static void format_block_ranges_mixed_eol(void)
{
    const char *text = "pre\n@block one\r\nA\r\n\n@block two\nZ";
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan(text, 1u), "format_ranges_mixed_eol_valid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, parser.block_count, "format_ranges_block_count");
    TEST_ASSERT_EQUAL_STRING("one", parser.blocks[0].id);
    TEST_ASSERT_EQUAL_UINT32(2u, parser.blocks[0].marker_line);
    TEST_ASSERT_EQUAL_UINT32(3u, parser.blocks[0].first_line);
    TEST_ASSERT_EQUAL_UINT32(4u, parser.blocks[0].last_line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(16u, parser.blocks[0].start, "format_range_start_after_crlf_marker");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20u, parser.blocks[0].end, "format_range_end_before_next_marker");
    TEST_ASSERT_EQUAL_UINT32(5u, parser.blocks[1].marker_line);
    TEST_ASSERT_EQUAL_UINT32(6u, parser.blocks[1].first_line);
    TEST_ASSERT_EQUAL_UINT32(6u, parser.blocks[1].last_line);
    TEST_ASSERT_EQUAL_UINT32(31u, parser.blocks[1].start);
    TEST_ASSERT_EQUAL_UINT32(32u, parser.blocks[1].end);
}

static void format_empty_blocks_and_unterminated_marker(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("@block a\n@block b\r\n@block c", 1u), "format_empty_blocks_valid");
    TEST_ASSERT_EQUAL_UINT32(3u, parser.block_count);
    for (uint32_t i = 0u; i < parser.block_count; ++i) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(parser.blocks[i].start, parser.blocks[i].end, "format_empty_block_zero_range");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, parser.blocks[i].first_line, "format_empty_block_first_zero");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, parser.blocks[i].last_line, "format_empty_block_last_zero");
    }
}

static void format_trailing_lf_does_not_invent_line(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("@block a\nA\n", 2u), "format_trailing_lf_valid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, parser.blocks[0].last_line, "format_trailing_lf_last_actual_line");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("@block a\n\n", 2u), "format_empty_content_line_valid");
    TEST_ASSERT_EQUAL_UINT32(2u, parser.blocks[0].first_line);
    TEST_ASSERT_EQUAL_UINT32(2u, parser.blocks[0].last_line);
    TEST_ASSERT_EQUAL_UINT32(1u, parser.blocks[0].end - parser.blocks[0].start);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("", 1u), "format_empty_file_valid");
    TEST_ASSERT_EQUAL_UINT32(0u, parser.block_count);
}

static void format_body_offset_preserves_physical_positions(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan_at((const uint8_t *)"@block a\n[[f#b]]", 16u, 1u, 17u, 3u), "format_body_origin_valid");
    TEST_ASSERT_EQUAL_UINT32(3u, parser.blocks[0].marker_line);
    TEST_ASSERT_EQUAL_UINT32(4u, parser.blocks[0].first_line);
    TEST_ASSERT_EQUAL_UINT32(26u, parser.blocks[0].start);
    TEST_ASSERT_EQUAL_UINT32(33u, parser.blocks[0].end);
    TEST_ASSERT_EQUAL_UINT32(26u, rows[0].start);
    TEST_ASSERT_EQUAL_UINT32(4u, rows[0].line);
}

static void format_reserved_marker_errors(void)
{
    const char *bad[] = {"@block", "@block ", "@block\tq", "@block  q", "@block q ", "@block q\t", "@block q.r", "@block q/r", "@block \xc3\xa9", "@block abcdefghijklmnopqrstuvwxyz123456"};
    for (uint32_t i = 0u; i < sizeof bad / sizeof bad[0]; ++i) {
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_FORMAT, scan(bad[i], 1u), "format_reserved_marker_bad_syntax");
    }
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_FORMAT, scan("@block\r\n", 1u), "format_reserved_marker_crlf_empty_id");
}

static void format_ordinary_marker_lookalikes(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("@blockchain\n @block a\n@blocks a\n@bloc\n@blockx\t\n", 1u), "format_lookalikes_are_text");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, parser.block_count, "format_lookalikes_declare_no_blocks");
}

static uint32_t make_blocks(uint32_t count)
{
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        int wrote = snprintf((char *)large + used, sizeof large - used, "@block b%02u\n", i);
        TEST_ASSERT_GREATER_THAN_INT(0, wrote);
        used += (uint32_t)wrote;
    }
    return used;
}

static void format_duplicate_and_case_identity(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_AMBIGUOUS, scan("@block a\nX\n@block a\n", 1u), "format_duplicate_is_ambiguous");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("@block a\n@block A\n", 1u), "format_ids_case_sensitive");
    TEST_ASSERT_EQUAL_UINT32(2u, parser.block_count);
}

static void format_duplicate_precedes_capacity(void)
{
    uint32_t used = make_blocks(64u);
    memcpy(large + used, "@block b00\n", 11u);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_AMBIGUOUS, scan_at(large, used + 11u, 17u, 0u, 1u), "format_duplicate_before_capacity");
}

static void format_64_limit_65_immediate(void)
{
    uint32_t used = make_blocks(64u);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan_at(large, used, 7u, 0u, 1u), "format_64_blocks_allowed");
    TEST_ASSERT_EQUAL_UINT32(64u, parser.block_count);
    used = make_blocks(65u);
    memcpy(large + used, "@block b00\n", 11u);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_LIMIT, scan_at(large, used + 11u, 4096u, 0u, 1u), "format_65th_distinct_immediate_limit");
}

static void format_references_without_blocks_and_source_identity(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("[[x.txt#a]][[x.txt#a]]\n@block NONE\n[[self#NONE]]", 1u), "format_references_valid");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(3u, row_count, "format_reference_occurrences_not_deduped");
    TEST_ASSERT_FALSE_MESSAGE(rows[0].has_source_id, "format_unnamed_reference_retained");
    TEST_ASSERT_EQUAL_STRING("x.txt", rows[0].target.name);
    TEST_ASSERT_EQUAL_STRING("a", rows[0].target.id);
    TEST_ASSERT_EQUAL_UINT32(0u, rows[0].start);
    TEST_ASSERT_EQUAL_UINT32(11u, rows[0].end);
    TEST_ASSERT_EQUAL_UINT32(11u, rows[1].start);
    TEST_ASSERT_TRUE_MESSAGE(rows[2].has_source_id, "format_named_reference_has_source_block");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("NONE", rows[2].source_id, "format_literal_none_id_is_not_absence");
    TEST_ASSERT_EQUAL_UINT32(3u, rows[2].line);
}

static void format_invalid_refs_are_literal(void)
{
    const char *invalid[] = {"[[a#b]", "[[a#b", "[[a#b|alias]]", "[[.#b]]", "[[..#b]]", "[[a/b#b]]", "[[a#b.c]]", "[[a#]]", "[[#b]]", "[[a#b\n]]", "[[a#b\r\n]]", "[[a#b c]]", "[[a#b#c]]", "[[a#abcdefghijklmnopqrstuvwxyz123456]]"};
    for (uint32_t i = 0u; i < sizeof invalid / sizeof invalid[0]; ++i) {
        row_count = 0u;
        TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan(invalid[i], 1u), "format_invalid_reference_is_literal");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, row_count, "format_invalid_reference_not_recorded");
    }
}

static void format_nested_refs_follow_left_to_right(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("[[[a#b]] [[outer [[c#d]] [[[[e#f]] [[x#y]][[z#w]]", 1u), "format_nested_refs_valid");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(5u, row_count, "format_nested_refs_complete_substrings");
    TEST_ASSERT_EQUAL_STRING("a", rows[0].target.name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, rows[0].start, "format_overlapping_brackets_earliest_legal");
    TEST_ASSERT_EQUAL_STRING("c", rows[1].target.name);
    TEST_ASSERT_EQUAL_STRING("e", rows[2].target.name);
    TEST_ASSERT_EQUAL_STRING("x", rows[3].target.name);
    TEST_ASSERT_EQUAL_STRING("z", rows[4].target.name);
}

static void format_refs_have_no_256_cap(void)
{
    for (uint32_t i = 0u; i < 300u; ++i) {
        memcpy(large + i * 7u, "[[a#b]]", 7u);
    }
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan_at(large, 2100u, 19u, 0u, 1u), "format_300_refs_stream_valid");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(300u, row_count, "format_refs_no_256_limit");
    TEST_ASSERT_EQUAL_UINT32(2093u, rows[299].start);
    TEST_ASSERT_EQUAL_UINT32(2100u, rows[299].end);
}

static void format_4096_boundaries_and_long_lines(void)
{
    memset(large, 'x', sizeof large);
    large[4092] = '\n';
    memcpy(large + 4093, "@block edge\r\n", 13u);
    memcpy(large + 8190, "[[a#b]]", 7u);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan_at(large, sizeof large, 4096u, 0u, 1u), "format_4096_marker_reference_long_line_valid");
    TEST_ASSERT_EQUAL_UINT32(1u, parser.block_count);
    TEST_ASSERT_EQUAL_UINT32(4106u, parser.blocks[0].start);
    TEST_ASSERT_EQUAL_UINT32(sizeof large, parser.blocks[0].end);
    TEST_ASSERT_EQUAL_UINT64(1u, row_count);
    TEST_ASSERT_EQUAL_UINT32(8190u, rows[0].start);
    TEST_ASSERT_EQUAL_STRING("edge", rows[0].source_id);
    large[4095] = 0xe4u;
    large[4096] = 0xb8u;
    large[4097] = 0xadu;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, text_check(large, sizeof large, 4096u), "format_4096_utf8_split_valid");
}

static void format_max_name_id_reference(void)
{
    uint8_t reference[67];
    memset(reference, 'a', sizeof reference);
    reference[0] = reference[1] = '[';
    reference[33] = '#';
    reference[65] = reference[66] = ']';
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan_at(reference, sizeof reference, 1u, 0u, 1u), "format_max_reference_valid");
    TEST_ASSERT_EQUAL_UINT64(1u, row_count);
    TEST_ASSERT_EQUAL_UINT32(31u, strlen(rows[0].target.name));
    TEST_ASSERT_EQUAL_UINT32(31u, strlen(rows[0].target.id));
    TEST_ASSERT_EQUAL_UINT32(67u, rows[0].end);
}

static void format_callback_reentry_and_stop(void)
{
    reenter = true;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_OK, scan("[[a#b]]", 1u), "format_callback_reentry_outer_valid");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_BUSY, callback_init, "format_callback_reentry_init_busy");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_BUSY, callback_feed, "format_callback_reentry_feed_busy");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_BUSY, callback_finish, "format_callback_reentry_finish_busy");
    refuse_row = true;
    row_count = 0u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_STOPPED, scan("[[a#b]][[c#d]]", 99u), "format_callback_refusal_stops");
    TEST_ASSERT_EQUAL_UINT64(1u, row_count);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_STOPPED, yan_block_parser_finish(&parser), "format_stopped_is_sticky");
}

static void format_api_arguments_alias_and_wrap(void)
{
    YanBlockAddress out, before;
    memset(&out, 0x5a, sizeof out);
    before = out;
    TEST_ASSERT_FALSE(yan_block_id_valid(NULL, 1u));
    TEST_ASSERT_FALSE(yan_block_id_valid((const uint8_t *)(UINTPTR_MAX - 1u), 3u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_address_parse(NULL, 0u, &out));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_address_parse((const uint8_t *)"a#b", 3u, NULL));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_address_parse((const uint8_t *)(UINTPTR_MAX - 1u), 3u, &out));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_address_parse((const uint8_t *)&out, 3u, &out));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_init(NULL));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_init((YanBlockTextValidator *)(UINTPTR_MAX - 1u)));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_feed(&validator, NULL, 0u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_init(&validator));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_feed(&validator, (const uint8_t *)&validator, 1u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_feed(&validator, (const uint8_t *)(UINTPTR_MAX - 1u), 3u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_init(NULL, 0u, 1u, NULL, NULL));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_init((YanBlockParser *)(UINTPTR_MAX - 1u), 0u, 1u, NULL, NULL));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_init(&parser, 0u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_parser_init(&parser, 0u, 1u, NULL, NULL));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_feed(&parser, (const uint8_t *)&parser, 1u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_feed(&parser, NULL, 1u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_feed(&parser, (const uint8_t *)(UINTPTR_MAX - 1u), 3u));
}

static void format_terminal_states_and_offset_overflow(void)
{
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_init(&validator));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_finish(&validator));
    YanBlockTextValidator saved = validator;
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_feed(&validator, NULL, 0u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_text_finish(&validator));
    TEST_ASSERT_EQUAL_MEMORY(&saved, &validator, sizeof saved);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_text_init(&validator));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID_TEXT, yan_block_text_feed(&validator, (const uint8_t *)"\0", 1u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID_TEXT, yan_block_text_finish(&validator));
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, scan("@block a\n", 1u));
    static YanBlockParser saved_parser;
    saved_parser = parser;
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_feed(&parser, NULL, 0u));
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID, yan_block_parser_finish(&parser));
    TEST_ASSERT_EQUAL_MEMORY(&saved_parser, &parser, sizeof parser);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK, yan_block_parser_init(&parser, UINT32_MAX, 1u, NULL, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(YAN_BLOCK_INVALID, yan_block_parser_feed(&parser, (const uint8_t *)"x", 1u), "format_file_offset_no_wrap");
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, parser.offset);
}

static void format_result_names(void)
{
    TEST_ASSERT_EQUAL_STRING("BLOCK_FORMAT", yan_block_result_name(YAN_BLOCK_FORMAT));
    TEST_ASSERT_EQUAL_STRING("AMBIGUOUS", yan_block_result_name(YAN_BLOCK_AMBIGUOUS));
    TEST_ASSERT_EQUAL_STRING("INVALID_TEXT", yan_block_result_name(YAN_BLOCK_INVALID_TEXT));
    TEST_ASSERT_EQUAL_STRING("METADATA", yan_block_result_name(YAN_BLOCK_METADATA));
    TEST_ASSERT_EQUAL_STRING("INVALID", yan_block_result_name((YanBlockResult)999));
}

static void format_reinit_publishes_only_fully_initialized_active_slots(void)
{
    uint32_t used=make_blocks(64u);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK,scan_at(large,used,13u,0u,1u));
    TEST_ASSERT_EQUAL_UINT32(64u,parser.block_count);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK,scan("@block abcdefghijklmnopqrstuvwxyz12345\n[[f#x]]",1u));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz12345",rows[0].source_id);
    memset(parser.blocks,0xa5,sizeof parser.blocks);
    row_count=0u;
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK,scan("@block a\n[[f#x]]",1u));
    TEST_ASSERT_EQUAL_UINT32(1u,parser.block_count);
    TEST_ASSERT_EQUAL_STRING("a",parser.blocks[0].id);
    for (uint32_t i=2u;i<sizeof parser.blocks[0].id;++i)
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u,(uint8_t)parser.blocks[0].id[i],"0040_active_id_tail_initialized");
    TEST_ASSERT_EQUAL_STRING("a",rows[0].source_id);
    for (uint32_t i=2u;i<sizeof rows[0].source_id;++i)
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u,(uint8_t)rows[0].source_id[i],"0040_source_id_tail_initialized");
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK,scan("@block a\n",3u));
    TEST_ASSERT_EQUAL_UINT32(0u,parser.blocks[0].first_line);
    TEST_ASSERT_EQUAL_UINT32(0u,parser.blocks[0].last_line);
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK,scan("",1u));
    TEST_ASSERT_EQUAL_UINT32(0u,parser.block_count);
    static YanBlockParser saved;
    saved=parser;
    TEST_ASSERT_EQUAL(YAN_BLOCK_INVALID,yan_block_parser_init(&parser,0u,0u,NULL,NULL));
    TEST_ASSERT_EQUAL_MEMORY(&saved,&parser,sizeof parser);
    parser.busy=true;saved=parser;
    TEST_ASSERT_EQUAL(YAN_BLOCK_BUSY,yan_block_parser_init(&parser,0u,1u,NULL,NULL));
    TEST_ASSERT_EQUAL_MEMORY(&saved,&parser,sizeof parser);
    parser.busy=false;refuse_row=true;
    TEST_ASSERT_EQUAL(YAN_BLOCK_STOPPED,scan("[[f#x]]",1u));
    refuse_row=false;
    TEST_ASSERT_EQUAL(YAN_BLOCK_OK,scan("@block a\n[[f#x]]",1u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(format_reinit_publishes_only_fully_initialized_active_slots);
    RUN_TEST(format_id_rules);
    RUN_TEST(format_address_rules_and_atomic_out);
    RUN_TEST(format_text_valid_chunks_and_feff);
    RUN_TEST(format_text_controls_and_bare_cr);
    RUN_TEST(format_text_invalid_utf8_and_truncation);
    RUN_TEST(format_text_late_damage_and_long_input);
    RUN_TEST(format_text_ascii_run_preserves_transition_state_and_bounds);
    RUN_TEST(format_block_ranges_mixed_eol);
    RUN_TEST(format_empty_blocks_and_unterminated_marker);
    RUN_TEST(format_trailing_lf_does_not_invent_line);
    RUN_TEST(format_body_offset_preserves_physical_positions);
    RUN_TEST(format_reserved_marker_errors);
    RUN_TEST(format_ordinary_marker_lookalikes);
    RUN_TEST(format_duplicate_and_case_identity);
    RUN_TEST(format_duplicate_precedes_capacity);
    RUN_TEST(format_64_limit_65_immediate);
    RUN_TEST(format_references_without_blocks_and_source_identity);
    RUN_TEST(format_invalid_refs_are_literal);
    RUN_TEST(format_nested_refs_follow_left_to_right);
    RUN_TEST(format_refs_have_no_256_cap);
    RUN_TEST(format_4096_boundaries_and_long_lines);
    RUN_TEST(format_max_name_id_reference);
    RUN_TEST(format_callback_reentry_and_stop);
    RUN_TEST(format_api_arguments_alias_and_wrap);
    RUN_TEST(format_terminal_states_and_offset_overflow);
    RUN_TEST(format_result_names);
    return UNITY_END();
}
