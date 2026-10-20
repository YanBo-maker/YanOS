/* Test-only link wrapper. The application's BLOCK_INIT handling stays real. */
#include "note_blocks.h"
YanBlockResult __wrap_yan_blocks_init(YanBlockContext *context, YanFs *fs, char (*names)[32]);
YanBlockResult __wrap_yan_blocks_init(YanBlockContext *context, YanFs *fs, char (*names)[32])
{
    (void)context;
    (void)fs;
    (void)names;
    return YAN_BLOCK_INVALID;
}
