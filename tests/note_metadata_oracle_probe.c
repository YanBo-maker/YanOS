/* Native observation adapter; all expectations live in the Python oracle. */
#include "note_metadata_format.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static uint8_t input[4096];
static YanNoteInfo info;
static YanNoteParseWorkspace workspace;
static uint8_t encoded[4096];
static uint32_t encoded_length;

static void hex_string(const uint8_t *bytes, uint32_t length)
{
    putchar('"');
    for (uint32_t i = 0u; i < length; ++i) {
        printf("%02x", (unsigned)bytes[i]);
    }
    putchar('"');
}

int main(void)
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        return 90;
    }
#endif
    uint8_t size[4];
    if (fread(size, 1u, sizeof size, stdin) != sizeof size) {
        return 91;
    }
    uint32_t length = (uint32_t)size[0] | ((uint32_t)size[1] << 8) |
                      ((uint32_t)size[2] << 16) | ((uint32_t)size[3] << 24);
    if (length > sizeof input || fread(input, 1u, length, stdin) != length) {
        return 92;
    }
    YanNoteResult result = yan_note_parse_header(input, length, &info, &workspace);
    if (result != YAN_NOTE_OK) {
        printf("{\"result\":\"%s\"}\n", yan_note_result_name(result));
        return 0;
    }
    if (info.tag_count > 16u || info.link_count > 16u ||
        info.title_length > 256u) {
        return 93;
    }
    printf("{\"result\":\"OK\",\"present\":%s,\"header_length\":%u,\"title_hex\":",
           info.has_header ? "true" : "false", (unsigned)info.header_bytes);
    if (info.has_title) {
        hex_string(info.title, info.title_length);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"created\":", stdout);
    if (info.has_created) {
        printf("\"%.10s\"", (const char *)info.created);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"modified\":", stdout);
    if (info.has_modified) {
        printf("\"%.10s\"", (const char *)info.modified);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"tags_hex\":[", stdout);
    for (uint32_t i = 0u; i < info.tag_count; ++i) {
        if (info.tag_length[i] > 63u) {
            return 94;
        }
        if (i != 0u) {
            putchar(',');
        }
        hex_string(info.tag[i], info.tag_length[i]);
    }
    fputs("],\"links\":[", stdout);
    for (uint32_t i = 0u; i < info.link_count; ++i) {
        if (info.link_length[i] > 31u ||
            !yan_note_name_valid(info.link[i], info.link_length[i])) {
            return 95;
        }
        if (i != 0u) {
            putchar(',');
        }
        printf("\"%.*s\"", (int)info.link_length[i], (const char *)info.link[i]);
    }
    fputs("],\"encoded_hex\":", stdout);
    if (info.has_header) {
        if (yan_note_encode_header(&info, encoded, sizeof encoded,
                                   &encoded_length) != YAN_NOTE_OK ||
            encoded_length > sizeof encoded) {
            return 97;
        }
        hex_string(encoded, encoded_length);
    } else {
        fputs("null", stdout);
    }
    fputs("}\n", stdout);
    return ferror(stdout) ? 96 : 0;
}
