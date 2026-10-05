/* yan_mkfs: the Host-side formatter for the YanFS on-disk layout.
 *
 * Contract: docs/specs/0021-yanfs.md, "格式化". The tool creates a brand-new
 * image with exclusive creation, writes capacity zero blocks, then rewrites
 * block 0 with the canonical empty directory produced by the pure encoder in
 * os/yanfs.c. It never formats an existing path, never repairs and never
 * retries: the filesystem core has no Host stdio and no format entry for the
 * Guest, so this is the only place the layout is written.
 *
 * Failure handling is part of the contract:
 *   - argument problems (unknown/duplicate/missing option, non-decimal or
 *     out-of-range capacity) exit 2 before anything is created;
 *   - Host create/write/seek/flush/close problems exit 5 and leave the partial
 *     new image in place for inspection; the target is never unlinked;
 *   - success is printed only after every write, flush and close succeeded.
 * The stdout line names the image and the capacity; stderr diagnostics are
 * stable so an external verifier can match them. */
#define _POSIX_C_SOURCE 200809L

#include "yanfs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs("usage: yan_mkfs --image FILE --blocks N\n", out);
    fputs("  --image FILE   new image path (must not already exist)\n", out);
    fputs("  --blocks N     capacity in 4096-byte blocks, 1..4294967295\n", out);
}

/* Decimal only: no sign, no whitespace, no base prefix, no fraction. The
 * accumulator cannot overflow because the function rejects as soon as the
 * value passes UINT32_MAX, long before a 64-bit wrap. */
static bool parse_capacity(const char *text, uint32_t *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        value = value * 10u + (uint64_t)(*cursor - '0');
        if (value > UINT32_MAX) {
            return false;
        }
    }
    if (value < 1u) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/* The capacity must also fit the Host file size the 0020 backend can express.
 * The format keeps capacity_blocks as u32 either way; 0020/0021 additionally
 * require the byte length to be representable by the Host C long, so the check
 * is the intersection of the C long bound and the off_t bound this build uses. */
static bool capacity_representable(uint32_t blocks)
{
    uint64_t bytes = (uint64_t)blocks * (uint64_t)YAN_FS_BLOCK_SIZE;
    if (bytes > (uint64_t)LONG_MAX) {
        return false;
    }
    if (sizeof(off_t) >= 8u) {
        return bytes <= (uint64_t)INT64_MAX;
    }
    return bytes <= (uint64_t)INT32_MAX;
}

int main(int argc, char **argv)
{
    const char *image = NULL;
    const char *blocks_text = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--image") == 0) {
            if (image != NULL || i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            image = argv[++i];
        } else if (strcmp(argv[i], "--blocks") == 0) {
            if (blocks_text != NULL || i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            blocks_text = argv[++i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (image == NULL || blocks_text == NULL) {
        usage(stderr);
        return 2;
    }
    uint32_t blocks = 0;
    if (!parse_capacity(blocks_text, &blocks) || !capacity_representable(blocks)) {
        fprintf(stderr, "yan_mkfs: invalid --blocks '%s'\n", blocks_text);
        return 2;
    }

    /* O_EXCL refuses an existing regular file and any symlink, including a
     * dangling one, so an existing path is never truncated or followed. */
    int fd = open(image, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr,
                    "yan_mkfs: refusing to overwrite '%s' (existing file or link)\n",
                    image);
        } else {
            fprintf(stderr, "yan_mkfs: cannot create '%s': %s\n", image,
                    strerror(errno));
        }
        return 5;
    }
    FILE *stream = fdopen(fd, "wb");
    if (stream == NULL) {
        fprintf(stderr, "yan_mkfs: cannot open stream for '%s': %s\n", image,
                strerror(errno));
        (void)close(fd);
        return 5;
    }

    /* One 4 KiB buffer, zero-filled by its static initializer, serves the zero
     * payload first and then the directory block. No sparse optimisation: the
     * whole capacity really moves through the stream. */
    static uint8_t buffer[YAN_FS_BLOCK_SIZE];
    int status = 0;

    for (uint32_t index = 0; index < blocks; ++index) {
        if (fwrite(buffer, 1u, sizeof buffer, stream) != sizeof buffer) {
            fprintf(stderr,
                    "yan_mkfs: short write to '%s' at block %" PRIu32 "\n",
                    image, index);
            status = 5;
            break;
        }
    }
    if (status == 0 && yan_fs_format_metadata(buffer, blocks) != YAN_FS_OK) {
        fprintf(stderr, "yan_mkfs: cannot build the empty directory for '%s'\n",
                image);
        status = 5;
    }
    if (status == 0 && fseeko(stream, (off_t)0, SEEK_SET) != 0) {
        fprintf(stderr, "yan_mkfs: seek to block 0 failed for '%s': %s\n", image,
                strerror(errno));
        status = 5;
    }
    if (status == 0 && fwrite(buffer, 1u, sizeof buffer, stream) != sizeof buffer) {
        fprintf(stderr, "yan_mkfs: short metadata write to '%s'\n", image);
        status = 5;
    }
    if (status == 0 && fflush(stream) != 0) {
        fprintf(stderr, "yan_mkfs: flush failed for '%s': %s\n", image,
                strerror(errno));
        status = 5;
    }
    if (fclose(stream) != 0) {
        fprintf(stderr, "yan_mkfs: close failed for '%s': %s\n", image,
                strerror(errno));
        status = 5;
    }
    if (status != 0) {
        return 5;
    }

    printf("yan_mkfs: wrote %s (%" PRIu32 " blocks)\n", image, blocks);
    return 0;
}
