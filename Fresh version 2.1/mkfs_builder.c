

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12


#pragma pack(push, 1)
typedef struct {
    uint32_t magic;               
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;
    uint64_t inode_count;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks; // 1
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;  // 1
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;

    uint64_t root_inode;          // 1
    uint64_t mtime_epoch;         
    uint32_t flags;               // 0
    uint32_t checksum;            
} superblock_t;


#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must be 116 bytes");

#pragma pack(push, 1)

typedef struct {
    uint16_t mode;         
    uint16_t links;        
    uint32_t uid;          // 0
    uint32_t gid;          // 0
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX]; 
    uint32_t reserved_0;   // 0
    uint32_t reserved_1;   // 0
    uint32_t reserved_2;   // 0
    uint32_t proj_id;      
    uint32_t uid16_gid16;  // 0
    uint64_t xattr_ptr;    // 0
    uint64_t inode_crc;    
} inode_t;

#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode must be 128 bytes");

#pragma pack(push, 1)
typedef struct {
    uint32_t inode_no;     
    uint8_t  type;         
    char     name[58];     
    uint8_t  checksum;     
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent must be 64 bytes");

//  helpers (taking from skeleton given temp 321): 

static uint32_t CRC32_TAB[256];
static void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
static uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint8_t block[BS]; memset(block, 0, BS);
    memcpy(block, sb, sizeof(*sb));
    uint32_t s = crc32(block, BS - 4);
    sb->checksum = s;
    return s;
}
static void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}
static void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}



//  helpers: 
static inline void set_bit(uint8_t* bmap, uint32_t idx){
    bmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}
static inline void zero_block(void* p){ memset(p, 0, BS); }

typedef struct { const char* image; uint32_t size_kib; uint32_t inodes; } cli_t;

static int parse_cli(int argc, char** argv, cli_t* c){
    memset(c, 0, sizeof(*c));
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--image") && i+1<argc) c->image = argv[++i];
        else if(!strcmp(argv[i],"--size-kib") && i+1<argc) c->size_kib = (uint32_t)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--inodes") && i+1<argc) c->inodes = (uint32_t)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr,"Unknown/invalid arg: %s\n", argv[i]); return -1; }
    }
    if(!c->image){ fprintf(stderr,"Missing --image\n"); return -1; }
    if(c->size_kib < 180 || c->size_kib > 4096 || (c->size_kib % 4)!=0){
        fprintf(stderr,"--size-kib must be in [180..4096] and multiple of 4\n"); return -1;
    }
    if(c->inodes < 128 || c->inodes > 512){
        fprintf(stderr,"--inodes must be in [128..512]\n"); return -1;
    }
    return 0;
}

int main(int argc, char** argv){
    crc32_init();
    cli_t cli; if(parse_cli(argc, argv, &cli)!=0) return 2;

    const uint64_t total_blocks = ((uint64_t)cli.size_kib * 1024u) / BS;
    const uint64_t inodes_per_blk = BS / INODE_SIZE;
    const uint64_t inode_tbl_blks = (cli.inodes + inodes_per_blk - 1) / inodes_per_blk;

    if(total_blocks < 3 + inode_tbl_blks + 1){
        fprintf(stderr,"Image too small: %u inodes need %" PRIu64 " blocks\n",
                cli.inodes, inode_tbl_blks);
        return 3;
    }

    // Allocating a full img in mem
    uint8_t* img = (uint8_t*)calloc((size_t)total_blocks, BS);
    if(!img){ perror("calloc"); return 1; }

    // Pointers to blocks
    uint8_t* blk0 = img + BS*0;  // superblock!
    
    uint8_t* blk1 = img + BS*1;  // inode bitmap!
    
    uint8_t* blk2 = img + BS*2;  // data bitmap!
    
    uint64_t inode_table_start = 3;
    uint64_t data_region_start  = 3 + inode_tbl_blks;
    uint64_t data_region_blocks = total_blocks - data_region_start;

    // - superblock things -
    
    superblock_t sb; memset(&sb, 0, sizeof(sb));
    sb.magic = 0x4D565346u; // 'MVSF'
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count  = cli.inodes;

    sb.inode_bitmap_start = 1;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = 2;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_blocks = inode_tbl_blks;
    sb.data_region_start = data_region_start;
    sb.data_region_blocks = data_region_blocks;

    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = (uint64_t)time(NULL);
    sb.flags = 0;
    superblock_crc_finalize(&sb);
    memset(blk0, 0, BS);
    memcpy(blk0, &sb, sizeof(sb));

    // - bitmaps things -
    memset(blk1, 0, BS);
    memset(blk2, 0, BS);
    
    // inode #1 used,
    set_bit(blk1, 0);
    
    // data region block #0 used, etai root dir
    set_bit(blk2, 0);

    
    // - inode table things-
    inode_t* itbl = (inode_t*)(img + BS*inode_table_start);
    memset(itbl, 0, (size_t)inode_tbl_blks * BS);

    inode_t root; memset(&root, 0, sizeof(root));
    root.mode  = 0040000;                // direc
    root.links = 2;                    // '.' and '..' curr and par
    root.uid = 0; root.gid = 0;
    root.atime = root.mtime = root.ctime = (uint64_t)time(NULL);
    root.size_bytes = 2 * sizeof(dirent64_t);
    root.direct[0] = (uint32_t)(data_region_start + 0);
    inode_crc_finalize(&root);
    itbl[0] = root;               // index 0 == inode #1

    // - root directory data thingss-
    
    uint8_t* rootblk = img + BS * root.direct[0];
    memset(rootblk, 0, BS);

    dirent64_t de; memset(&de, 0, sizeof(de));
    
    // "." curr
    de.inode_no = ROOT_INO; de.type = 2;
    memset(de.name, 0, sizeof(de.name));
    de.name[0] = '.';
    dirent_checksum_finalize(&de);
    memcpy(rootblk + 0*sizeof(dirent64_t), &de, sizeof(de));

    
    // ".."par
    memset(&de, 0, sizeof(de));
    de.inode_no = ROOT_INO; de.type = 2;
    de.name[0] = '.'; de.name[1] = '.';
    dirent_checksum_finalize(&de);
    memcpy(rootblk + 1*sizeof(dirent64_t), &de, sizeof(de));

    // - write image
    FILE* f = fopen(cli.image, "wb");
    if(!f){ perror("fopen"); free(img); return 5; }
    size_t wrote = fwrite(img, BS, (size_t)total_blocks, f);
    fclose(f);
    free(img);
    if(wrote != (size_t)total_blocks){
        fprintf(stderr,"Short write: wrote %zu blocks\n", wrote);
        return 6;
    }
    fprintf(stdout,"Created MiniVSFS image '%s' (%" PRIu64 " blocks, %u inodes)\n",
            cli.image, total_blocks, cli.inodes);
    return 0;
}
