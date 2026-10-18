/* RV32 checks use a real mounted YanFS and callbacks, not forged busy state. */
#include "guest.h"
#include "platform.h"
#include "note_blocks.h"

static YanFs fs;
static YanBlockContext blocks;
static YanBlockSummary summary;
static uint8_t disk[8][4096];
static uint32_t reads, writes, callbacks, callback_reads;
static YanBlockResult reentry, reinit;
static YanFsIoResult capacity(void *c, uint64_t *n)
{ (void)c; *n = 8u; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *c, uint32_t lba, uint8_t *out)
{
    (void)c; ++reads;
    if (lba >= 8u) return YAN_FS_IO_ERROR;
    memcpy(out, disk[lba], 4096u); return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *c, uint32_t lba, const uint8_t *in)
{
    (void)c; ++writes;
    if (lba >= 8u) return YAN_FS_IO_ERROR;
    memcpy(disk[lba], in, 4096u); return YAN_FS_IO_OK;
}
static bool accept(void *c, const YanBlockRow *row)
{ (void)c; (void)row; ++callbacks; return true; }
static bool stop(void *c, const YanBlockRow *row)
{
    (void)c; ++callbacks;
    guest_check(row->block.id[0] == 'a' && row->block.first_line == 2u, 30u);
    callback_reads = reads;
    reentry = yan_blocks_list(&blocks, NULL, NULL, NULL, NULL);
    reinit = yan_blocks_init(&blocks, &fs);
    guest_check(reads == callback_reads && writes == 0u, 31u);
    return false;
}
static bool chunk(void *c, uint32_t offset, const uint8_t *bytes, uint32_t length)
{
    (void)c;
    guest_check(offset == 0u && length == 9u && bytes[0] == '[', 32u);
    return true;
}

int main(void)
{
    YanFsBlockIo io = {NULL, capacity, read_block, write_block};
    guest_check(yan_fs_init(&fs, io) == YAN_FS_OK, 2u);
    guest_check(yan_fs_format_metadata(disk[0], 8u) == YAN_FS_OK, 3u);
    guest_check(yan_fs_mount(&fs) == YAN_FS_OK, 4u);
    static const uint8_t content[] = "@block a\n[[n#a]]\r\n@block empty";
    guest_check(yan_fs_create(&fs, "n", content, sizeof content - 1u) == YAN_FS_OK, 5u);
    guest_check(yan_blocks_init(&blocks, &fs) == YAN_BLOCK_OK, 6u);
    reads = writes = 0u;
    memset(&summary, 0xA5, sizeof summary);
    YanBlockSummary original; memcpy(&original, &summary, sizeof original);
    YanBlockContext *wrap = (YanBlockContext *)(uintptr_t)(UINTPTR_MAX - sizeof blocks + 1u);
    YanBlockAddress address = {{'n', 0}, {'a', 0}};
    guest_check(yan_blocks_list(wrap, "n", accept, NULL, &summary) == YAN_BLOCK_INVALID, 7u);
    guest_check(yan_blocks_init(wrap, &fs) == YAN_BLOCK_INVALID, 8u);
    guest_check(yan_blocks_init(&blocks, (YanFs *)(uintptr_t)0xFFFFFFFEu) == YAN_BLOCK_INVALID, 9u);
    guest_check(yan_blocks_backlinks(&blocks, &address, accept, NULL,
                (YanBlockSummary *)(uintptr_t)0xFFFFFFFEu) == YAN_BLOCK_INVALID, 10u);
    guest_check(yan_blocks_list(&blocks, (const char *)(uintptr_t)UINTPTR_MAX,
                accept, NULL, &summary) == YAN_BLOCK_INVALID, 11u);
    guest_check(yan_blocks_list(&blocks, (const char *)&blocks, accept, NULL, &summary) == YAN_BLOCK_INVALID, 12u);
    guest_check(yan_blocks_list(&blocks, "n", accept, NULL,
                (YanBlockSummary *)(void *)&fs) == YAN_BLOCK_INVALID, 13u);
    guest_check(yan_blocks_list(&blocks, "n", accept, &blocks, &summary) == YAN_BLOCK_INVALID, 14u);
    guest_check(yan_blocks_list(&blocks, "n", accept, &fs, &summary) == YAN_BLOCK_INVALID, 15u);
    guest_check(yan_blocks_list(&blocks, (const char *)&summary, accept, NULL, &summary) == YAN_BLOCK_INVALID, 16u);
    guest_check(yan_blocks_backlinks(&blocks, (const YanBlockAddress *)(uintptr_t)0xFFFFFFFEu,
                accept, NULL, &summary) == YAN_BLOCK_INVALID, 17u);
    guest_check(yan_blocks_list(&blocks, "n", accept, NULL,
                (YanBlockSummary *)(void *)((uint8_t *)&blocks + sizeof blocks - 1u)) == YAN_BLOCK_INVALID, 18u);
    guest_check(reads == 0u && writes == 0u && callbacks == 0u, 19u);
    guest_check(memcmp(&original, &summary, sizeof summary) == 0, 20u);
    guest_check(yan_blocks_list(&blocks, "n", stop, NULL, &summary) == YAN_BLOCK_STOPPED, 21u);
    guest_check(callbacks == 1u && reentry == YAN_BLOCK_BUSY && reinit == YAN_BLOCK_BUSY, 22u);
    guest_check(reads == callback_reads && writes == 0u, 23u);
    guest_check(memcmp(&original, &summary, sizeof summary) == 0, 24u);
    guest_check(yan_blocks_show(&blocks, &address, accept, chunk, NULL, &summary) == YAN_BLOCK_OK, 25u);
    guest_check(summary.total == 1u && summary.shown == 1u, 26u);
    guest_check(yan_blocks_refs(&blocks, "n", accept, NULL, &summary) == YAN_BLOCK_OK, 27u);
    guest_check(summary.total == 1u && writes == 0u && !blocks.busy, 28u);
    address.name[0] = 'x';
    guest_check(yan_blocks_backlinks(&blocks, &address, accept, NULL, &summary) == YAN_BLOCK_OK, 29u);
    guest_check(summary.total == 0u && summary.target_state == YAN_BLOCK_DANGLING_FILE, 33u);
    guest_check(yan_fs_unmount(&fs) == YAN_FS_OK, 34u);
    uint32_t before = reads;
    guest_check(yan_blocks_list(&blocks, NULL, NULL, NULL, NULL) == YAN_BLOCK_NOT_MOUNTED, 35u);
    guest_check(reads == before && writes == 0u, 36u);
    const char *marker = "NOTE_BLOCKS_API_PASS";
    while (*marker) { while (!yan_os_uart_tx_ready()) {} (void)yan_os_uart_put((uint8_t)*marker++); }
    guest_finish(1u); return 0;
}
