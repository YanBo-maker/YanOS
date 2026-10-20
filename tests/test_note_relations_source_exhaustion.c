/* Compile YanFS with an exhausted identity source; relation queries need no cache token. */
#include "note_relations.h"
#include "fs_test_support.h"
#include "unity.h"
#include <string.h>
static YanFs fs;
static YanRelationContext relation;
static uint8_t disk[4][4096];
static uint32_t callbacks, writes;
static YanFsIoResult capacity(void *c, uint64_t *n) {(void)c;*n=4u;return YAN_FS_IO_OK;}
static YanFsIoResult read_block(void *c,uint32_t lba,uint8_t *out) {
    (void)c;if(lba>=4u)return YAN_FS_IO_ERROR;memcpy(out,disk[lba],4096u);return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *c,uint32_t lba,const uint8_t *in) {
    (void)c;++writes;if(lba>=4u)return YAN_FS_IO_ERROR;memcpy(disk[lba],in,4096u);return YAN_FS_IO_OK;
}
static bool row(void *c,const YanRelationRow *r) {(void)c;(void)r;++callbacks;return true;}
void setUp(void) {memset(&fs,0,sizeof fs);memset(&relation,0,sizeof relation);memset(disk,0,sizeof disk);callbacks=writes=0u;}
void tearDown(void) {}
static void exhausted_identity_still_allows_all_relation_queries(void) {
    YanFsBlockIo io={NULL,capacity,read_block,write_block};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,yan_fs_format_metadata(disk[0],4u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,test_fs_init(&fs,io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,yan_fs_mount(&fs));
    static const uint8_t content[]="@yan-note 1\nlink: n\n@end\n";
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,yan_fs_create(&fs,"n",content,sizeof content-1u));
    YanFsSource token;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,yan_fs_source(&fs,&token));
    TEST_ASSERT_FALSE_MESSAGE(token.cacheable,"identity_really_exhausted");
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,test_relation_init(&relation,&fs));
    writes=0u;
    YanRelationSummary summary;
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,yan_relation_links(&relation,"n",row,NULL,&summary));
    TEST_ASSERT_EQUAL_UINT32(1u,summary.total);
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,yan_relation_backlinks(&relation,"n",row,NULL,&summary));
    TEST_ASSERT_EQUAL_UINT32(1u,summary.total);
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,yan_relation_related(&relation,"n",row,NULL,&summary));
    TEST_ASSERT_EQUAL_UINT32(0u,summary.total);
    TEST_ASSERT_EQUAL_UINT32(2u,callbacks);
    TEST_ASSERT_EQUAL_UINT32(0u,writes);
}
int main(void) {UNITY_BEGIN();RUN_TEST(exhausted_identity_still_allows_all_relation_queries);return UNITY_END();}
