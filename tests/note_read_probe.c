/* Host yan_fs_read observation for the 0035 MCP write checks. */
#include "semantic_posix.h"
#include "yanfs.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s IMAGE NAME\n", argv[0]);
        return 2;
    }
    YanSemanticPosix adapter = {0};
    adapter.image_fd = adapter.index_fd = -1;
    if (yan_semantic_posix_open_image_ex(&adapter, argv[1]) != YAN_IMAGE_OPEN_OK)
        return 3;
    YanFsInfo info;
    if (yan_fs_stat(&adapter.fs, argv[2], &info) != YAN_FS_OK) return 4;
    uint8_t *bytes = info.size_bytes ? malloc(info.size_bytes) : NULL;
    if (info.size_bytes && !bytes) return 5;
    uint32_t got = 0;
    YanFsResult read = yan_fs_read(&adapter.fs, argv[2], 0, bytes, info.size_bytes,
                                   &got);
    bool ok = read == YAN_FS_OK && got == info.size_bytes;
    if (ok && got) ok = fwrite(bytes, 1, got, stdout) == got;
    free(bytes);
    if (yan_semantic_posix_cleanup(&adapter) != YAN_SEMANTIC_OK) ok = false;
    return ok ? 0 : 6;
}
