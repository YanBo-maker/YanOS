/* RV32 public relation API checks using a real FS, without patching its state. */
#include "guest.h"
#include "../fs_test_support.h"
#include "platform.h"
#include "note_relations.h"

static YanFs fs;
static YanRelationContext relation;
static YanRelationRow rows[YAN_FS_MAX_FILES];
static uint16_t relation_index[YAN_FS_MAX_FILES];
static YanRelationSummary summary;
static uint8_t disk[8][4096];
static uint32_t reads, writes, callbacks;
static YanNoteResult reentry, reinit;
static uint32_t callback_reads;
static YanFsIoResult capacity(void *c, uint64_t *n) {
    (void)c; *n=8u; return YAN_FS_IO_OK;
}
static YanFsIoResult read_block(void *c, uint32_t lba, uint8_t *out) {
    (void)c; ++reads; if(lba>=8u) return YAN_FS_IO_ERROR;
    memcpy(out,disk[lba],4096u); return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *c, uint32_t lba, const uint8_t *in) {
    (void)c; ++writes; if(lba>=8u) return YAN_FS_IO_ERROR;
    memcpy(disk[lba],in,4096u); return YAN_FS_IO_OK;
}
static bool stop(void *c, const YanRelationRow *row) {
    (void)c; ++callbacks;
    guest_check(row->name[0]=='n' && row->name[1]=='\0' && row->exists,30u);
    guest_check(!row->outgoing && !row->incoming && row->shared_tags==0u,32u);
    callback_reads=reads;
    reentry=yan_relation_related(&relation,NULL,NULL,NULL,NULL);
    reinit=yan_relation_init(&relation,&fs,rows,relation_index);
    guest_check(reads==callback_reads && writes==0u,31u);
    return false;
}
static bool accept(void *c, const YanRelationRow *row) {
    (void)c; (void)row; ++callbacks; return true;
}

int main(void) {
    YanFsBlockIo io={NULL,capacity,read_block,write_block};
    guest_check(test_fs_init(&fs,io)==YAN_FS_OK,2u);
    guest_check(yan_fs_format_metadata(disk[0],8u)==YAN_FS_OK,3u);
    guest_check(yan_fs_mount(&fs)==YAN_FS_OK,4u);
    static const uint8_t content[]="@yan-note 1\nlink: n\ntag: os\n@end\n\000binary";
    guest_check(yan_fs_create(&fs,"n",content,sizeof content-1u)==YAN_FS_OK,5u);
    guest_check(yan_relation_init(&relation,&fs,rows,relation_index)==YAN_NOTE_OK,6u);
    reads=writes=0u;
    summary.total=0xdeadbeefu; summary.shown=0xa5a5u; summary.target_exists=true;
    YanRelationContext *wrap=(YanRelationContext *)(uintptr_t)(UINTPTR_MAX-sizeof relation+1u);
    guest_check(yan_relation_links(wrap,"n",accept,NULL,&summary)==YAN_NOTE_INVALID,7u);
    guest_check(yan_relation_init(wrap,&fs,rows,relation_index)==YAN_NOTE_INVALID,8u);
    guest_check(yan_relation_init(&relation,(YanFs *)(uintptr_t)0xfffffffeu,rows,relation_index)==YAN_NOTE_INVALID,9u);
    guest_check(yan_relation_backlinks(&relation,"n",accept,NULL,(YanRelationSummary *)(uintptr_t)0xfffffffeu)==YAN_NOTE_INVALID,10u);
    guest_check(yan_relation_related(&relation,(const char *)(uintptr_t)UINTPTR_MAX,accept,NULL,&summary)==YAN_NOTE_INVALID,11u);
    guest_check(yan_relation_links(&relation,(const char *)&relation,accept,NULL,&summary)==YAN_NOTE_INVALID,12u);
    guest_check(yan_relation_links(&relation,"n",accept,NULL,(YanRelationSummary *)(void *)&fs)==YAN_NOTE_INVALID,13u);
    guest_check(yan_relation_links(&relation,"n",accept,&relation,&summary)==YAN_NOTE_INVALID,14u);
    guest_check(yan_relation_links(&relation,"n",accept,&fs,&summary)==YAN_NOTE_INVALID,15u);
    guest_check(yan_relation_links(&relation,(const char *)&summary,accept,NULL,&summary)==YAN_NOTE_INVALID,16u);
    guest_check(reads==0u && writes==0u && callbacks==0u,17u);
    guest_check(summary.total==0xdeadbeefu && summary.shown==0xa5a5u && summary.target_exists,18u);
    guest_check(yan_relation_links(&relation,"n",stop,NULL,&summary)==YAN_NOTE_STOPPED,19u);
    guest_check(callbacks==1u && reentry==YAN_NOTE_BUSY && reinit==YAN_NOTE_BUSY,20u);
    guest_check(reads==callback_reads && writes==0u,21u);
    guest_check(summary.total==0xdeadbeefu && summary.shown==0xa5a5u && summary.target_exists,22u);
    guest_check(yan_relation_related(&relation,"n",accept,NULL,NULL)==YAN_NOTE_OK,23u);
    guest_check(callbacks==1u,24u); /* self is excluded, NULL summary is legal */
    guest_check(yan_relation_backlinks(&relation,"missing",accept,NULL,&summary)==YAN_NOTE_OK,25u);
    guest_check(summary.total==0u && summary.shown==0u && !summary.target_exists,26u);
    static const char long_name[]="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxz";
    guest_check(sizeof long_name==32u,33u);
    guest_check(yan_relation_backlinks(&relation,long_name,accept,NULL,&summary)==YAN_NOTE_OK,34u);
    struct { char name[2]; YanRelationSummary value; } adjacent;
    adjacent.name[0]='n'; adjacent.name[1]='\0';
    guest_check(yan_relation_backlinks(&relation,adjacent.name,accept,NULL,&adjacent.value)==YAN_NOTE_OK,35u);
    guest_check(adjacent.value.total==1u && adjacent.value.shown==1u && adjacent.value.target_exists,36u);
    guest_check(yan_fs_unmount(&fs)==YAN_FS_OK,27u);
    uint32_t before=reads;
    guest_check(yan_relation_links(&relation,NULL,NULL,NULL,NULL)==YAN_NOTE_NOT_MOUNTED,28u);
    guest_check(reads==before && writes==0u,29u);
    const char *marker="NOTE_RELATIONS_API_PASS";
    while(*marker) { while(!yan_os_uart_tx_ready()) {} (void)yan_os_uart_put((uint8_t)*marker++); }
    guest_finish(1u); return 0;
}
