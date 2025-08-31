// save as minivsfs_ls.c, then: gcc -O2 -std=c17 minivsfs_ls.c -o minivsfs_ls
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define BS 4096u
#define INODE_SIZE 128u
#define DIRECT_MAX 12

#pragma pack(push,1)
typedef struct {
    uint32_t magic, version, block_size;
    uint64_t total_blocks, inode_count;
    uint64_t inode_bitmap_start, inode_bitmap_blocks;
    uint64_t data_bitmap_start,  data_bitmap_blocks;
    uint64_t inode_table_start,  inode_table_blocks;
    uint64_t data_region_start,  data_region_blocks;
    uint64_t root_inode, mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;

typedef struct {
    uint16_t mode, links;
    uint32_t uid, gid;
    uint64_t size_bytes, atime, mtime, ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0, reserved_1, reserved_2;
    uint32_t proj_id, uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;

typedef struct {
    uint32_t inode_no;
    uint8_t  type;
    char     name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)

int main(int argc, char** argv){
    if(argc!=2){ fprintf(stderr,"Usage: %s <image>\n", argv[0]); return 1; }
    FILE* f=fopen(argv[1],"rb"); if(!f){perror("open"); return 1;}
    superblock_t sb={0};
    if(fread(&sb,1,sizeof(sb),f)!=sizeof(sb)){fprintf(stderr,"sb read fail\n");return 1;}
    if(sb.magic!=0x4D565346u||sb.block_size!=BS){fprintf(stderr,"Not MiniVSFS\n");return 2;}
    printf("MiniVSFS: blocks=%llu, inodes=%llu, inode_tbl=[%llu..%llu), data_region_start=%llu\n",
        (unsigned long long)sb.total_blocks,(unsigned long long)sb.inode_count,
        (unsigned long long)sb.inode_table_start,
        (unsigned long long)(sb.inode_table_start+sb.inode_table_blocks),
        (unsigned long long)sb.data_region_start);

    // read inode #1 (index 0)
    if(fseek(f, (long)(sb.inode_table_start*BS), SEEK_SET)!=0){perror("seek itbl");return 3;}
    inode_t ino; if(fread(&ino,1,sizeof(ino),f)!=sizeof(ino)){fprintf(stderr,"inode read fail\n");return 3;}
    printf("root: links=%u, size=%llu bytes, first data blk=%u\n",
        ino.links,(unsigned long long)ino.size_bytes, ino.direct[0]);

    // dump first root dir block
    if(ino.direct[0]==0){ printf("root has no data block?\n"); return 0; }
    if(fseek(f, (long)(ino.direct[0]*BS), SEEK_SET)!=0){perror("seek rootblk");return 4;}
    for(int i=0;i<(int)(BS/sizeof(dirent64_t));i++){
        dirent64_t de; if(fread(&de,1,sizeof(de),f)!=sizeof(de)) break;
        if(de.inode_no==0) continue;
        printf("entry[%03d]: ino=%u type=%u name='%.*s'\n",
            i, de.inode_no, de.type, 58, de.name);
    }
    fclose(f); return 0;
}
