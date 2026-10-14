/* Binary Host driver for the independent relation reference; real public FS. */
#include "note_relations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#define BLOCKS 256u
static uint8_t medium[BLOCKS][4096];
static uint8_t data[65536];
static YanFs fs;
static YanRelationContext relation;
static unsigned reads, writes;
static YanFsIoResult capacity(void *opaque, uint64_t *out)
{ (void)opaque; *out=BLOCKS; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *opaque, uint32_t lba, uint8_t out[4096])
{ (void)opaque; ++reads; if(lba>=BLOCKS)return YAN_FS_IO_ERROR; memcpy(out,medium[lba],4096);return YAN_FS_IO_OK; }
static YanFsIoResult write_block(void *opaque,uint32_t lba,const uint8_t in[4096])
{ (void)opaque; ++writes;if(lba>=BLOCKS)return YAN_FS_IO_ERROR;memcpy(medium[lba],in,4096);return YAN_FS_IO_OK; }
static void get_bytes(void *out, uint32_t size)
{ if(fread(out,1,size,stdin)!=size)exit(90); }
static uint32_t get32(void)
{ uint8_t b[4];get_bytes(b,4);return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static bool row(void *opaque,const YanRelationRow *value)
{
    (void)opaque;
    printf("R\t%s\t%u\t%u\t%u\t%u\n",value->name,(unsigned)value->exists,
           (unsigned)value->outgoing,(unsigned)value->incoming,(unsigned)value->shared_tags);
    return true;
}
int main(int argc,char **argv)
{
#ifdef _WIN32
    if(_setmode(_fileno(stdin),_O_BINARY)==-1)return 88;
#endif
    if(argc!=2)return 89;
    char target[32]={0};uint32_t length=get32();
    if(length>31)return 90;
    get_bytes(target,length);
    uint32_t count=get32();if(count>63)return 90;
    if(yan_fs_format_metadata(medium[0],BLOCKS)!=YAN_FS_OK)return 91;
    YanFsBlockIo io={NULL,capacity,read_block,write_block};
    if(yan_fs_init(&fs,io)!=YAN_FS_OK||yan_fs_mount(&fs)!=YAN_FS_OK)return 91;
    for(uint32_t i=0;i<count;++i){
        char name[32]={0};uint32_t nl=get32(),size=get32();
        if(nl>31||size>sizeof data)return 90;
        get_bytes(name,nl);get_bytes(data,size);
        if(yan_fs_create(&fs,name,data,size)!=YAN_FS_OK)return 91;
    }
    if(yan_relation_init(&relation,&fs)!=YAN_NOTE_OK)return 91;
    reads=writes=0;
    YanRelationSummary summary={UINT32_C(0xdeadbeef),UINT32_C(0xa5a5),true};
    YanNoteResult result;
    if(strcmp(argv[1],"links")==0)result=yan_relation_links(&relation,target,row,NULL,&summary);
    else if(strcmp(argv[1],"backlinks")==0)result=yan_relation_backlinks(&relation,target,row,NULL,&summary);
    else if(strcmp(argv[1],"related")==0)result=yan_relation_related(&relation,target,row,NULL,&summary);
    else return 89;
    printf("Q\t%u\t%u\t%u\t%u\t%u\t%u\n",(unsigned)result,summary.total,summary.shown,
           (unsigned)summary.target_exists,reads,writes);
    return ferror(stdout)?92:0;
}
