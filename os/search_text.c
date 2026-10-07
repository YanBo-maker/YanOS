/* Shared text primitives for the 0026 term search.
 *
 * Pure functions only: no I/O, no YanFS, no console and no libc beyond the
 * fixed-width types. The decoder is a straight RFC 3629 check on the smallest
 * legal encodings, because the term grammar compares whole scalars and must not
 * mistake a truncated multi-byte sequence for legal content. The exact range
 * checks matter: the first continuation byte of a two-byte sequence must be
 * 0x80..0xBF, while the lead byte ranges exclude the overlong forms by
 * construction. */
#include "search_text.h"

#include <stddef.h>

uint32_t yan_search_text_utf8_scalar(const uint8_t *bytes, uint32_t length,
                                     uint32_t *code_point)
{
    if (bytes == NULL || length == 0u) {
        return 0u;
    }
    uint8_t lead = bytes[0];
    if (lead < 0x80u) {
        if (code_point != NULL) {
            *code_point = lead;
        }
        return 1u;
    }
    if (lead < 0xc2u) {
        return 0u; /* continuation byte or overlong two-byte lead */
    }
    if (lead < 0xe0u) {
        if (length < 2u || (bytes[1] & 0xc0u) != 0x80u) {
            return 0u;
        }
        if (code_point != NULL) {
            *code_point = ((uint32_t)(lead & 0x1fu) << 6) |
                          (uint32_t)(bytes[1] & 0x3fu);
        }
        return 2u;
    }
    if (lead < 0xf0u) {
        if (length < 3u || (bytes[1] & 0xc0u) != 0x80u ||
            (bytes[2] & 0xc0u) != 0x80u) {
            return 0u;
        }
        /* E0 A0..BF excludes overlong; ED 80..9F excludes surrogates. */
        if (lead == 0xe0u && bytes[1] < 0xa0u) {
            return 0u;
        }
        if (lead == 0xedu && bytes[1] > 0x9fu) {
            return 0u;
        }
        if (code_point != NULL) {
            *code_point = ((uint32_t)(lead & 0x0fu) << 12) |
                          ((uint32_t)(bytes[1] & 0x3fu) << 6) |
                          (uint32_t)(bytes[2] & 0x3fu);
        }
        return 3u;
    }
    if (lead < 0xf5u) {
        if (length < 4u || (bytes[1] & 0xc0u) != 0x80u ||
            (bytes[2] & 0xc0u) != 0x80u || (bytes[3] & 0xc0u) != 0x80u) {
            return 0u;
        }
        /* F0 90..BF excludes overlong; F4 80..8F excludes > U+10FFFF. */
        if (lead == 0xf0u && bytes[1] < 0x90u) {
            return 0u;
        }
        if (lead == 0xf4u && bytes[1] > 0x8fu) {
            return 0u;
        }
        if (code_point != NULL) {
            *code_point = ((uint32_t)(lead & 0x07u) << 18) |
                          ((uint32_t)(bytes[1] & 0x3fu) << 12) |
                          ((uint32_t)(bytes[2] & 0x3fu) << 6) |
                          (uint32_t)(bytes[3] & 0x3fu);
        }
        return 4u;
    }
    return 0u;
}

uint32_t yan_search_text_utf8_expected(const uint8_t *bytes, uint32_t length)
{
    if (bytes == NULL || length == 0u) {
        return 0u;
    }
    uint8_t lead = bytes[0];
    if (lead < 0x80u) {
        return 1u;
    }
    if (lead < 0xc2u) {
        return 0u; /* continuation byte or overlong two-byte lead */
    }
    uint32_t need;
    if (lead < 0xe0u) {
        need = 2u;
    } else if (lead < 0xf0u) {
        need = 3u;
    } else if (lead < 0xf5u) {
        need = 4u;
    } else {
        return 0u;
    }
    /* Every continuation byte that is already available must be well formed. */
    for (uint32_t i = 1u; i < length && i < need; ++i) {
        if ((bytes[i] & 0xc0u) != 0x80u) {
            return 0u;
        }
    }
    /* The range checks that only need the second byte rule out the overlong and
     * surrogate forms as soon as that byte arrives. */
    if (length >= 2u) {
        if (need == 3u) {
            if (lead == 0xe0u && bytes[1] < 0xa0u) {
                return 0u;
            }
            if (lead == 0xedu && bytes[1] > 0x9fu) {
                return 0u;
            }
        } else if (need == 4u) {
            if (lead == 0xf0u && bytes[1] < 0x90u) {
                return 0u;
            }
            if (lead == 0xf4u && bytes[1] > 0x8fu) {
                return 0u;
            }
        }
    }
    return need;
}

bool yan_search_text_utf8_valid(const uint8_t *bytes, uint32_t length)
{
    if (length > 0u && bytes == NULL) {
        return false;
    }
    uint32_t offset = 0u;
    while (offset < length) {
        uint32_t need = yan_search_text_utf8_expected(bytes + offset,
                                                      length - offset);
        if (need == 0u || need > length - offset) {
            return false;
        }
        offset += need;
    }
    return true;
}

bool yan_search_text_is_ascii_word(uint8_t byte)
{
    return (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
           (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
           (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') || byte == (uint8_t)'_';
}

uint8_t yan_search_text_ascii_fold(uint8_t byte)
{
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') {
        return (uint8_t)(byte + ((uint8_t)'a' - (uint8_t)'A'));
    }
    return byte;
}
