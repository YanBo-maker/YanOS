/* RV32 0027 service guards over a real YanFS with a static in-memory device.
 * Near-uintptr addresses are rejected before dereference; no private FS state
 * is patched. Each failure has a unique tohost code; success also prints a marker. */
#include "guest.h"
#include "../fs_test_support.h"
#include "platform.h"
#include "note_metadata.h"
#include "search_terms_index.h"
static YanFs fs;
static YanNoteContext note;
static YanNoteInfo info;
static YanNoteTagFilter filter;
static YanSearchTermsIndex index_terms;
static YanNoteEntry note_entries[YAN_FS_MAX_FILES];
static char note_sort[YAN_FS_MAX_FILES][32];
static uint16_t note_sort_index[YAN_FS_MAX_FILES];
static YanFsInfo index_entries[YAN_FS_MAX_FILES];
static uint8_t index_eligible[YAN_FS_MAX_FILES];
static uint8_t disk[8][4096];
static uint32_t reads, writes, callbacks;
static YanNoteResult reentry;
static YanFsIoResult capacity(void *c, uint64_t *n) { (void)c; *n=8u; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *c, uint32_t lba, uint8_t *out) {
    (void)c; ++reads; if(lba>=8u) return YAN_FS_IO_ERROR;
    memcpy(out,disk[lba],4096u); return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *c, uint32_t lba, const uint8_t *in) {
    (void)c; ++writes; if(lba>=8u) return YAN_FS_IO_ERROR;
    memcpy(disk[lba],in,4096u); return YAN_FS_IO_OK;
}
static bool stop(void *c, const char *name) {
    (void)c; (void)name; ++callbacks;
    reentry=yan_note_show(&note,"n",&info); return false;
}
static bool match(void *c, const YanSearchTermsMatch *m) { (void)c; (void)m; return true; }
int main(void) {
    YanFsBlockIo io={NULL,capacity,read_block,write_block};
    guest_check(test_fs_init(&fs,io)==YAN_FS_OK,2u);
    guest_check(yan_fs_format_metadata(disk[0],8u)==YAN_FS_OK,3u);
    guest_check(yan_fs_mount(&fs)==YAN_FS_OK,4u);
    guest_check(yan_fs_create(&fs,"n",(const uint8_t *)"body\r\n",6u)==YAN_FS_OK,5u);
    guest_check(yan_note_init(&note,&fs,note_entries,note_sort,note_sort_index)==YAN_NOTE_OK,6u);
    reads=writes=0u;
    YanNoteContext *wrap=(YanNoteContext *)(uintptr_t)(UINTPTR_MAX-sizeof note+1u);
    guest_check(yan_note_show(wrap,"n",&info)==YAN_NOTE_INVALID,7u);
    guest_check(yan_note_init(wrap,&fs,note_entries,note_sort,note_sort_index)==YAN_NOTE_INVALID,8u);
    guest_check(yan_note_init(&note,(YanFs *)(uintptr_t)0xfffffffeu,note_entries,note_sort,note_sort_index)==YAN_NOTE_INVALID,9u);
    guest_check(yan_note_show(&note,"n",(YanNoteInfo *)(uintptr_t)0xfffffffeu)==YAN_NOTE_INVALID,10u);
    guest_check(yan_note_set_title(&note,"n",(const uint8_t *)(uintptr_t)0xfffffffeu,10u)==YAN_NOTE_INVALID,11u);
    guest_check(yan_note_show(&note,(const char *)&note,&info)==YAN_NOTE_INVALID,12u);
    guest_check(yan_note_show(&note,"n",(YanNoteInfo *)(void *)&fs)==YAN_NOTE_INVALID,13u);
    guest_check(yan_note_list(&note,(YanNoteTagFilter *)(uintptr_t)0xfffffffeu,stop,NULL,NULL)==YAN_NOTE_INVALID,14u);
    filter.count=17u;
    guest_check(yan_note_list(&note,&filter,stop,NULL,NULL)==YAN_NOTE_INVALID,15u);
    guest_check(yan_note_list(&note,NULL,stop,NULL,(uint32_t *)(uintptr_t)0xfffffffeu)==YAN_NOTE_INVALID,16u);
    guest_check(reads==0u && writes==0u,17u);
    guest_check(yan_note_tag(&note,"n",YAN_NOTE_MANAGE_ADD,(const uint8_t *)"os",2u)==YAN_NOTE_OK,18u);
    guest_check(yan_note_show(&note,"n",&info)==YAN_NOTE_OK && info.header_bytes==25u && info.tag_count==1u,19u);
    uint32_t shown=0xdeadbeefu;
    uint32_t before=reads;
    guest_check(yan_note_list(&note,NULL,stop,NULL,&shown)==YAN_NOTE_STOPPED,20u);
    guest_check(callbacks==1u && reentry==YAN_NOTE_BUSY && shown==0xdeadbeefu,21u);
    guest_check(reads==before+1u,22u);
    before=writes;
    guest_check(yan_note_tag(&note,"n",YAN_NOTE_MANAGE_ADD,(const uint8_t *)"os",2u)==YAN_NOTE_OK && writes==before,23u);
    static uint8_t body[32]; uint32_t got=0u;
    guest_check(yan_fs_read(&fs,"n",25u,body,sizeof body,&got)==YAN_FS_OK && got==6u && memcmp(body,"body\r\n",6u)==0,24u);
    guest_check(yan_search_terms_index_init(&index_terms,&fs,index_entries,index_eligible)==YAN_SEARCH_TERMS_OK,25u);
    YanSearchTermsBackend backend=yan_search_terms_index_backend(&index_terms);
    filter.count=1u; filter.length[0]=2u; filter.tag[0][0]='o';filter.tag[0][1]='s';
    before=reads;
    void *index_wrap=(void *)(uintptr_t)(UINTPTR_MAX-sizeof index_terms+1u);
    guest_check(backend.query_filtered(index_wrap,(const uint8_t *)"body",4u,&filter,match,NULL,NULL)==YAN_SEARCH_TERMS_INVALID,26u);
    guest_check(reads==before,27u);
    const char *marker="NOTE_METADATA_API_PASS";
    while(*marker) { while(!yan_os_uart_tx_ready()) {} (void)yan_os_uart_put((uint8_t)*marker++); }
    guest_finish(1u); return 0;
}
