/* Test-only link wrapper. The application's BLOCK_INIT handling stays real. */
#include "note_blocks.h"
YanBlockResult __wrap_yan_blocks_init(YanBlockContext *context, YanFs *fs);
YanBlockResult __wrap_yan_blocks_init(YanBlockContext *context, YanFs *fs)
{
    (void)context;
    (void)fs;
    return YAN_BLOCK_INVALID;
}
