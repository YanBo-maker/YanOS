/* Test-only fault injector for the 0044 R2 CLI failure contract.
 *
 * The production binary has no fault switch and none is added: this translation
 * unit is linked only into `yan_graph_fault_probe` and alters behaviour through
 * the linker's `--wrap`, so the real `tools/yan_graph.c` runs unchanged.
 *
 * Modes, all selected by the `YAN_KG_FAULT` environment variable:
 *
 *   stale        the SECOND source-identity digest returns a different value.
 *                This exercises the comparison branch only; it does not prove
 *                that a real image change is observed.
 *   rewrite      after the FIRST real cleanup, mutate one dedicated throw-away
 *                v1 image's directory entry (rename one file) and recompute its
 *                CRC. The tool's own second adapter then re-reads the directory
 *                block and digests the changed logical source, so STALE comes
 *                from a genuinely different source rather than a mocked digest.
 *                The path comes from `YAN_KG_FAULT_IMAGE`; only that file is
 *                touched, and only its intended metadata bytes.
 *   close        the FIRST cleanup reports an error after really closing.
 *   close-second the SECOND cleanup reports an error after really closing, so
 *                the post-observation source close is what fails.
 *
 * `__real_*` is always called, so every descriptor is genuinely closed and no
 * real work is skipped; only the reported result is altered on the selected
 * ordinal. The injector is the actor, not the production writer.
 */
#define _POSIX_C_SOURCE 200809L

#include "semantic_corpus.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define YAN_KG_FAULT_BLOCK_SIZE 4096u
#define YAN_KG_FAULT_CRC_OFFSET 60u
#define YAN_KG_FAULT_ENTRY_FIRST 64u
#define YAN_KG_FAULT_ENTRY_SIZE 64u
#define YAN_KG_FAULT_SLOTS 63u

static int observations;
static int cleanups;

static bool mode_is(const char *expected)
{
    const char *current = getenv("YAN_KG_FAULT");
    return current != NULL && strcmp(current, expected) == 0;
}

/* The 0021/0036 directory CRC: IEEE reflected CRC32 over the whole (here: one
 * block) directory with the CRC field itself treated as zero. Small and
 * independent of the production filesystem, which this file only reads. */
static uint32_t directory_crc(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t index = 0u; index < length; ++index) {
        bool in_field = index >= YAN_KG_FAULT_CRC_OFFSET &&
                        index < YAN_KG_FAULT_CRC_OFFSET + 4u;
        crc ^= (uint32_t)(in_field ? 0u : bytes[index]);
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ UINT32_C(0xEDB88320)
                                   : crc >> 1;
        }
    }
    return crc ^ UINT32_C(0xFFFFFFFF);
}

static void store_le32(uint8_t *out, uint32_t value)
{
    for (unsigned index = 0u; index < 4u; ++index) {
        out[index] = (uint8_t)(value >> (8u * index));
    }
}

/* Renames the first occupied entry of a v1 image and reseals it: one bounded,
 * deterministic field change, so the directory bytes and the logical source
 * identity both differ while the on-disk format stays valid. */
static bool rename_first_entry(const char *path)
{
    uint8_t block[YAN_KG_FAULT_BLOCK_SIZE];
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return false;
    }
    if (pread(fd, block, sizeof block, 0) != (ssize_t)sizeof block) {
        (void)close(fd);
        return false;
    }
    bool changed = false;
    for (uint32_t slot = 0u; slot < YAN_KG_FAULT_SLOTS && !changed; ++slot) {
        uint8_t *entry = block + YAN_KG_FAULT_ENTRY_FIRST +
                         (size_t)slot * YAN_KG_FAULT_ENTRY_SIZE;
        if (entry[0] == 0u) {
            continue;
        }
        /* Only the first name byte moves, to another legal YanFS name byte, so
         * the length and every other field stay untouched. */
        entry[0] = entry[0] == (uint8_t)'a' ? (uint8_t)'b' : (uint8_t)'a';
        store_le32(block + YAN_KG_FAULT_CRC_OFFSET,
                   directory_crc(block, sizeof block));
        changed = pwrite(fd, block, sizeof block, 0) == (ssize_t)sizeof block;
    }
    if (close(fd) != 0) {
        return false;
    }
    return changed;
}

YanSemanticResult __real_yan_semantic_source_digest(const YanSemanticSource *,
                                                    uint8_t out[32]);
YanSemanticResult __wrap_yan_semantic_source_digest(const YanSemanticSource *source,
                                                    uint8_t out[32]);
YanSemanticResult __wrap_yan_semantic_source_digest(const YanSemanticSource *source,
                                                    uint8_t out[32])
{
    const bool stale = mode_is("stale");
    ++observations;
    if (stale && observations >= 2) {
        YanSemanticResult result = __real_yan_semantic_source_digest(source, out);
        if (result == YAN_SEMANTIC_OK) {
            out[0] ^= 0xFFu;
        }
        return result;
    }
    return __real_yan_semantic_source_digest(source, out);
}

YanSemanticResult __real_yan_semantic_posix_cleanup(void *);
YanSemanticResult __wrap_yan_semantic_posix_cleanup(void *);
YanSemanticResult __wrap_yan_semantic_posix_cleanup(void *opaque)
{
    /* The real close always runs, so no descriptor is left behind; only the
     * reported result of the selected ordinal is altered. */
    YanSemanticResult result = __real_yan_semantic_posix_cleanup(opaque);
    ++cleanups;
    if (mode_is("rewrite") && cleanups == 1) {
        const char *path = getenv("YAN_KG_FAULT_IMAGE");
        if (path != NULL && path[0] != '\0') {
            (void)rename_first_entry(path);
        }
        return result;
    }
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (mode_is("close") && cleanups == 1) {
        return YAN_SEMANTIC_IO;
    }
    if (mode_is("close-second") && cleanups == 2) {
        return YAN_SEMANTIC_IO;
    }
    return result;
}
