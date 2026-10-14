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
 * remain owned while a later dependency or mount check fails. cleanup closes
 * it, clears the fd, and is idempotent; CLI does this before returning. */
typedef struct {
    YanFs fs;
    int image_fd, index_fd;
    FILE *temporary;
    uint64_t image_size;
    dev_t input_dev[3];
    ino_t input_ino[3];
    const char *index_path;
    char *temporary_path;
    bool owns_temporary;
} YanSemanticPosix;

YanSemanticResult yan_semantic_posix_open(YanSemanticPosix *, const char *image,
    const char *index, const char *model, const char *runner,
    YanSemanticIdentity *identity);
void yan_semantic_posix_bind(YanSemanticPosix *, YanSemanticSource *, YanSemanticStore *);
YanSemanticResult yan_semantic_posix_close_source(void *);
YanSemanticResult yan_semantic_posix_cleanup(YanSemanticPosix *);

#endif
