#ifndef YAN_OS_NOTE_BLOCKS_FORMAT_H
#define YAN_OS_NOTE_BLOCKS_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

/* Pure streaming grammar for 0032. No filesystem, shell, Host or allocation.
 * Callers first qualify the COMPLETE file as editor-compatible text, then
 * exclude a valid 0027 header and feed only its body to the structural parser.
 * Offsets remain absolute file bytes and lines remain physical file lines.
 * A parser's growing table and callbacks are streaming progress, not atomic
 * publication; the read-only service supplies the preflight/publish boundary.
 */

#define YAN_BLOCK_ID_MAX UINT32_C(31)
#define YAN_BLOCK_NAME_MAX UINT32_C(31)
#define YAN_BLOCK_MAX_COUNT UINT32_C(64)

typedef enum {
    YAN_BLOCK_OK = 0,
    YAN_BLOCK_INVALID,
    YAN_BLOCK_BUSY,
    YAN_BLOCK_STOPPED,
    YAN_BLOCK_METADATA,
    YAN_BLOCK_FORMAT,
    YAN_BLOCK_AMBIGUOUS,
    YAN_BLOCK_LIMIT,
    YAN_BLOCK_INVALID_TEXT,
    YAN_BLOCK_NOT_MOUNTED,
    YAN_BLOCK_FAULTED,
    YAN_BLOCK_NOT_FOUND,
    YAN_BLOCK_IO,
    YAN_BLOCK_PROTOCOL,
    YAN_BLOCK_CORRUPT,
    YAN_BLOCK_UNSUPPORTED
} YanBlockResult;

typedef struct {
    char name[32];
    char id[32];
} YanBlockAddress;

typedef struct {
    char id[32];
    uint32_t marker_line;
    uint32_t first_line;
    uint32_t last_line;
    uint32_t start;
    uint32_t end;
} YanBlockDecl;

typedef struct {
    YanBlockAddress target;
    bool has_source_id;
    char source_id[32];
    uint32_t line;
    uint32_t start;
    uint32_t end;
} YanBlockReference;

typedef bool (*YanBlockReferenceSink)(void *context,
                                      const YanBlockReference *reference);

/* Both objects must be zero initialized before first init, remain caller
 * owned, and not overlap any offered input span. Keep the parser in long-lived
 * storage, not the Guest's 4 KiB task stack. The workspace fields below are
 * public for sizeof/static ownership; only blocks/block_count are results.
 * first/last_line are both zero for an empty range. */
typedef struct {
    uint32_t initialized;
    YanBlockResult result;
    uint8_t utf8[4];
    uint8_t utf8_length;
    bool pending_cr;
    bool finished;
} YanBlockTextValidator;

typedef struct {
    YanBlockDecl blocks[YAN_BLOCK_MAX_COUNT];
    uint32_t block_count;
    uint32_t initialized;
    YanBlockResult result;
    uint32_t offset;
    uint64_t line;
    uint32_t line_start;
    uint8_t line_prefix[40];
    uint8_t prefix_length;
    uint8_t last_byte;
    uint8_t ref_state;
    uint8_t name_length;
    uint8_t id_length;
    bool finished;
    bool busy;
    YanBlockReference reference;
    YanBlockReferenceSink ref_sink;
    void *ref_context;
} YanBlockParser;

_Static_assert(sizeof(YanBlockParser) <= 4096u,
               "block grammar workspace must remain bounded");

/* Length-based ASCII validation. Address parse writes *out only on success;
 * bytes/out may not overlap and all declared spans must fit uintptr_t. */
bool yan_block_id_valid(const uint8_t *bytes, uint32_t length);
YanBlockResult yan_block_address_parse(const uint8_t *bytes, uint32_t length,
                                       YanBlockAddress *out);
const char *yan_block_result_name(YanBlockResult result);

/* Init resets an idle object. Feed accepts NULL only at zero length; invalid
 * arguments preserve the object. Content errors and callback STOPPED are
 * sticky until init. After successful finish, feed/finish return INVALID and
 * preserve the completed object. A partial UTF-8 scalar or pending CR fails
 * qualification at finish, including when it spans separate feed calls. */
YanBlockResult yan_block_text_init(YanBlockTextValidator *validator);
YanBlockResult yan_block_text_feed(YanBlockTextValidator *validator,
                                   const uint8_t *bytes, uint32_t length);
YanBlockResult yan_block_text_finish(YanBlockTextValidator *validator);

/* body_offset and first_line identify the first body byte (line is >=1).
 * The scanner assumes qualified text; it does not parse metadata or rerun
 * text validation. The optional sink receives every legal reference in byte
 * order, with no occurrence cap. Its reference is synchronously borrowed and
 * may not be retained. Returning false gives sticky STOPPED. Reentry into
 * init/feed/finish on the active parser gives BUSY without changing it. The
 * opaque context is passed through; its undeclared object size is not guessed.
 * Actual file ranges are checked against UINT32_MAX before adding feed bytes;
 * the internal line counter is u64 so a final LF never wraps a physical line.
 */
YanBlockResult yan_block_parser_init(YanBlockParser *parser,
                                     uint32_t body_offset,
                                     uint32_t first_line,
                                     YanBlockReferenceSink sink,
                                     void *sink_context);
YanBlockResult yan_block_parser_feed(YanBlockParser *parser,
                                     const uint8_t *bytes, uint32_t length);
YanBlockResult yan_block_parser_finish(YanBlockParser *parser);

#endif
