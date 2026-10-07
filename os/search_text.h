#ifndef YAN_OS_SEARCH_TEXT_H
#define YAN_OS_SEARCH_TEXT_H

#include <stdbool.h>
#include <stdint.h>

/* Shared text primitives for 0026 term search.
 *
 * Both the linear scan backend and the in-memory index use these so the
 * ASCII/token boundary and UTF-8 rules cannot drift between the two execution
 * paths. The header includes no YanFS, console or Host header and allocates
 * nothing, so the same source compiles for the Guest.
 *
 * The rules are the ones docs/specs/0026-term-search-index.md fixes: ASCII
 * letters, digits and underscore form a complete word (case folded only for
 * comparison); every other byte belongs to the raw byte stream and non-ASCII
 * content is matched as whole UTF-8 scalars. No locale, no Unicode case
 * folding, no dictionary segmentation. */

/* Decodes one UTF-8 scalar at the start of [bytes, bytes+length). On success
 * writes the code point to *code_point (when non-NULL) and returns the number
 * of bytes consumed, 1..4. Returns 0 for a truncated or invalid sequence,
 * including overlong encodings, UTF-16 surrogates and scalars above U+10FFFF,
 * so a caller can stop at the first byte of a bad sequence. */
uint32_t yan_search_text_utf8_scalar(const uint8_t *bytes, uint32_t length,
                                     uint32_t *code_point);

/* Total byte length of the UTF-8 scalar whose lead byte is bytes[0], judged
 * from the bytes available so far. Returns 1..4 when [bytes, bytes+length) is a
 * legal prefix of a scalar (the sequence may still need the caller to supply
 * more bytes), and 0 when the available bytes can never be part of a legal
 * scalar. A streaming validator appends one byte at a time and consumes the
 * returned length as soon as it has that many bytes, so an incomplete sequence
 * at a chunk boundary is carried instead of being mistaken for damage. */
uint32_t yan_search_text_utf8_expected(const uint8_t *bytes, uint32_t length);

/* True when the whole [bytes, bytes+length) buffer is a sequence of valid UTF-8
 * scalars. An empty buffer is valid. */
bool yan_search_text_utf8_valid(const uint8_t *bytes, uint32_t length);

/* True for [A-Za-z0-9_], the only bytes that continue an ASCII word. */
bool yan_search_text_is_ascii_word(uint8_t byte);

/* Folds one byte with ASCII-only case mapping; every other byte is unchanged. */
uint8_t yan_search_text_ascii_fold(uint8_t byte);

#endif
