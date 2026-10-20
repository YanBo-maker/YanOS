#ifndef YAN_HOST_SEMANTIC_POSIX_H
#define YAN_HOST_SEMANTIC_POSIX_H

#include "semantic.h"
#include "yanfs.h"
#include <stdio.h>
#include <sys/types.h>

/* Linux adapter only. It opens the existing image strictly read-only; the
 * write callback is an unconditional refusal. Paths are borrowed through
 * cleanup, and the index temporary is always in the target's directory.
 * Initialize fd fields to -1 before the first call. The caller must invoke
 * cleanup after every open return, including errors: an image descriptor can
 * remain owned while a later dependency or mount check fails. cleanup is the
 * close path for both open variants, clears every fd, and is idempotent. It
 * reports the first close error it observes (image before index). cleanup
 * cannot see an operation result that happened before it was called, so the
 * caller keeps that earlier error and lets cleanup replace only an otherwise
 * OK result. CLI does this before returning. */
typedef struct {
    YanFs fs;
    /* 0036-R3: the directory cache is calloc'd for the image's D blocks and
     * released by cleanup. */
    uint8_t *directory;
    int image_fd, index_fd;
    FILE *temporary;
    uint64_t image_size;
    dev_t input_dev[3];
    ino_t input_ino[3];
    const char *index_path;
    char *temporary_path;
    bool owns_temporary;
} YanSemanticPosix;

/* Lossless image-only open result. The legacy yan_semantic_posix_open_image
 * and yan_semantic_posix_open keep their historical YanSemanticResult mapping;
 * this typed seam exists so the MCP worker can distinguish an unsupported
 * on-disk format from corrupt image structure. */
typedef enum {
    YAN_IMAGE_OPEN_OK = 0,
    YAN_IMAGE_OPEN_INVALID,
    YAN_IMAGE_OPEN_IO,
    YAN_IMAGE_OPEN_CORRUPT,
    YAN_IMAGE_OPEN_UNSUPPORTED,
    YAN_IMAGE_OPEN_NOT_MOUNTED,
    YAN_IMAGE_OPEN_FAULTED,
    YAN_IMAGE_OPEN_BUSY,
    YAN_IMAGE_OPEN_PROTOCOL
} YanImageOpenResult;

YanSemanticResult yan_semantic_posix_open(YanSemanticPosix *, const char *image,
    const char *index, const char *model, const char *runner,
    YanSemanticIdentity *identity);

/* 0033 image-only entry. Validates p/image (non-null, non-empty), zero
 * initializes the adapter, sets image_fd/index_fd to -1 and opens only the
 * image O_RDONLY|O_CLOEXEC|O_NONBLOCK. It never stats, opens or hashes the
 * index, model or runner, so those paths may be absent; on success fs is
 * mounted, image size and dev/ino are recorded, index_path stays NULL and no
 * index temporary exists. The adapter is a read source, not a store. As with
 * open, call cleanup after every return, including errors. */
YanSemanticResult yan_semantic_posix_open_image(YanSemanticPosix *,
                                                const char *image);

/* Typed variant for the MCP block path. It validates and opens exactly like
 * yan_semantic_posix_open_image but reports YanFs mount failures losslessly:
 * UNSUPPORTED stays UNSUPPORTED and NOT_MOUNTED/FAULTED/BUSY/PROTOCOL stay
 * distinct. The caller still invokes cleanup after every return. */
YanImageOpenResult yan_semantic_posix_open_image_ex(YanSemanticPosix *,
                                                    const char *image);

/* Binds only the enumerated/read source; it does not require an index and
 * never binds a store. Use yan_semantic_posix_bind when both parts are
 * wanted. An image-only adapter must not be used through a store. */
void yan_semantic_posix_bind_source(YanSemanticPosix *, YanSemanticSource *);

void yan_semantic_posix_bind(YanSemanticPosix *, YanSemanticSource *, YanSemanticStore *);
YanSemanticResult yan_semantic_posix_close_source(void *);
YanSemanticResult yan_semantic_posix_cleanup(YanSemanticPosix *);

#endif
