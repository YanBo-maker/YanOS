/* Test-only link wrapper. The application's BLOCK_INIT handling stays real. */
#include "note_blocks.h"
YanBlockResult __wrap_yan_blocks_init(YanBlockContext *context, YanFs *fs, char (*names)[32], uint16_t *sort_index);
YanBlockResult __wrap_yan_blocks_init(YanBlockContext *context, YanFs *fs, char (*names)[32], uint16_t *sort_index)
{
    (void)context;
    (void)fs;
    (void)names;
    (void)sort_index;
    return YAN_BLOCK_INVALID;
}
