// mkfs_adder_refactored.c
// Refactor of your mkfs_adder to match the MiniVSFS spec exactly.
// - Validates 116B superblock with magic 0x4D565346 & block_size==4096
// - First-fit inode & data allocation inside data region
// - Adds filename (base name, ≤58 bytes) to /, extends root if needed
// - Updates bitmaps, inodes (CRC), root links (+1), root size (+64 if new dirent)
// - CLI: --input <in.img> --output <out.img> --file <path>

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;

    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must be 116 bytes");

#pragma pack(push, 1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
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

// ---------------------- CRC32 helpers (from skeleton) ----------------------
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

// ---------------------- helpers ----------------------
static inline int test_bit(const uint8_t* bmap, uint32_t idx){
    return (bmap[idx >> 3] >> (idx & 7)) & 1u;
}
static inline void set_bit(uint8_t* bmap, uint32_t idx){
    bmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

typedef struct { const char* in_img; const char* out_img; const char* filepath; } cli_t;

static int parse_cli(int argc, char** argv, cli_t* c){
    memset(c, 0, sizeof(*c));
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--input") && i+1<argc) c->in_img = argv[++i];
        else if(!strcmp(argv[i],"--output") && i+1<argc) c->out_img = argv[++i];
        else if(!strcmp(argv[i],"--file") && i+1<argc) c->filepath = argv[++i];
        else { fprintf(stderr,"Unknown/invalid arg: %s\n", argv[i]); return -1; }
    }
    if(!c->in_img || !c->out_img || !c->filepath){
        fprintf(stderr,"Usage: --input <img> --output <img> --file <path>\n");
        return -1;
    }
    return 0;
}

static int read_entire(FILE* f, uint8_t** buf_out, size_t* bytes_out){
    if(fseek(f, 0, SEEK_END)!=0) return -1;
    long sz = ftell(f);
    if(sz<0) return -1;
    if(fseek(f, 0, SEEK_SET)!=0) return -1;
    uint8_t* b = (uint8_t*)malloc((size_t)sz);
    if(!b) return -1;
    if(fread(b,1,(size_t)sz,f)!=(size_t)sz){ free(b); return -1; }
    *buf_out = b; *bytes_out = (size_t)sz; return 0;
}

static const char* base_name(const char* path){
    const char* s = strrchr(path, '/');
#ifdef _WIN32
    const char* b = strrchr(path, '\\');
    if(!s || (b && b > s)) s = b;
#endif
    return s ? s+1 : path;
}

int main(int argc, char** argv){
    crc32_init();
    cli_t cli; if(parse_cli(argc, argv, &cli)!=0) return 2;

    // Read input image
    FILE* fi = fopen(cli.in_img, "rb");
    if(!fi){ perror("fopen input"); return 1; }
    uint8_t* img = NULL; size_t img_bytes = 0;
    if(read_entire(fi, &img, &img_bytes)!=0){ fclose(fi); fprintf(stderr,"Failed to read input image\n"); return 1; }
    fclose(fi);

    if(img_bytes % BS){ fprintf(stderr,"Invalid image (size not multiple of block size)\n"); free(img); return 1; }
    const uint64_t total_blocks = img_bytes / BS;

    // Map SB and validate
    superblock_t* sb = (superblock_t*)(img + BS*0);
    if(sb->magic != 0x4D565346u || sb->version != 1 || sb->block_size != BS){
        fprintf(stderr,"Not a MiniVSFS image\n"); free(img); return 3;
    }
    if(sb->total_blocks != total_blocks){
        fprintf(stderr,"Superblock total_blocks mismatch\n"); free(img); return 3;
    }

    uint8_t* inode_bmap = img + BS * sb->inode_bitmap_start;
    uint8_t* data_bmap  = img + BS * sb->data_bitmap_start;
    inode_t* itbl       = (inode_t*)(img + BS * sb->inode_table_start);

    // Read file to add
    struct stat st;
    if(stat(cli.filepath,&st)!=0){ perror("stat --file"); free(img); return 4; }
    if(!S_ISREG(st.st_mode)){ fprintf(stderr,"--file must be a regular file\n"); free(img); return 4; }
    uint64_t fsize = (uint64_t)st.st_size;
    uint32_t need_blocks = (uint32_t)((fsize + BS - 1) / BS);
    if(need_blocks > DIRECT_MAX){
        fprintf(stderr,"File too large for 12 direct blocks (max 49152 bytes)\n"); free(img); return 5;
    }

    // First-fit free inode (0..inode_count-1)
    uint32_t new_ino_idx = UINT32_MAX;
    for(uint32_t i=0;i<(uint32_t)sb->inode_count;i++){
        if(!test_bit(inode_bmap, i)){ new_ino_idx = i; break; }
    }
    if(new_ino_idx==UINT32_MAX){ fprintf(stderr,"No free inodes\n"); free(img); return 6; }
    uint32_t new_ino_no = new_ino_idx + 1;

    // First-fit free data blocks in data region
    uint32_t* db_idxs = NULL;
    if(need_blocks){
        db_idxs = (uint32_t*)malloc(sizeof(uint32_t)*need_blocks);
        if(!db_idxs){ free(img); return 1; }
        uint32_t found=0;
        for(uint32_t i=0;i<(uint32_t)sb->data_region_blocks && found<need_blocks;i++){
            if(!test_bit(data_bmap, i)){ db_idxs[found++] = i; }
        }
        if(found < need_blocks){
            fprintf(stderr,"Not enough free data blocks\n"); free(db_idxs); free(img); return 6;
        }
    }

    // Allocate bits
    set_bit(inode_bmap, new_ino_idx);
    for(uint32_t i=0;i<need_blocks;i++) set_bit(data_bmap, db_idxs[i]);

    // Build inode
    inode_t* ino = &itbl[new_ino_idx];
    memset(ino, 0, sizeof(*ino));
    ino->mode  = 0100000; // regular file
    ino->links = 1;
    ino->uid = 0; ino->gid = 0;
    ino->size_bytes = fsize;
    ino->atime = ino->mtime = ino->ctime = (uint64_t)time(NULL);
    for(uint32_t i=0;i<need_blocks;i++){
        ino->direct[i] = (uint32_t)(sb->data_region_start + db_idxs[i]);
    }
    inode_crc_finalize(ino);

    // Write file data
    if(need_blocks){
        FILE* ff = fopen(cli.filepath, "rb");
        if(!ff){ perror("open --file"); free(db_idxs); free(img); return 4; }
        for(uint32_t i=0;i<need_blocks;i++){
            uint8_t* blk = img + BS * ino->direct[i];
            memset(blk, 0, BS);
            size_t toread = (i+1<need_blocks)? BS : (size_t)(fsize - (uint64_t)i*BS);
            if(toread>0 && fread(blk,1,toread,ff) != toread){
                perror("read --file"); fclose(ff); free(db_idxs); free(img); return 4;
            }
        }
        fclose(ff);
    }

    // Add directory entry into root
    inode_t* root = &itbl[0]; // inode #1
    uint64_t now = (uint64_t)time(NULL);
    const char* base = base_name(cli.filepath);
    char namebuf[58]; memset(namebuf, 0, sizeof(namebuf));
    size_t namelen = strlen(base);
    if(namelen > sizeof(namebuf)) namelen = sizeof(namebuf);
    memcpy(namebuf, base, namelen);

    int placed = 0;
    for(int d=0; d<DIRECT_MAX && !placed; d++){
        if(root->direct[d]==0) break;
        uint8_t* blk = img + BS*root->direct[d];
        for(int i=0;i<(int)(BS/sizeof(dirent64_t));i++){
            dirent64_t* e = (dirent64_t*)(blk + i*sizeof(dirent64_t));
            if(e->inode_no==0){
                dirent64_t ne; memset(&ne,0,sizeof(ne));
                ne.inode_no = new_ino_no;
                ne.type = 1;
                memcpy(ne.name, namebuf, namelen);
                dirent_checksum_finalize(&ne);
                memcpy(e, &ne, sizeof(ne));
                root->size_bytes += sizeof(dirent64_t);
                root->mtime = root->ctime = now;
                root->links += 1; // per spec
                inode_crc_finalize(root);
                placed = 1; break;
            }
        }
    }
    if(!placed){
        // Need to extend root with a new data block
        int slot = -1;
        for(int d=0; d<DIRECT_MAX; d++){
            if(root->direct[d]==0){ slot = d; break; }
        }
        if(slot < 0){
            fprintf(stderr,"Root directory has no free direct pointer to extend\n");
            free(db_idxs); free(img); return 7;
        }
        // find a free data block
        uint32_t free_idx = UINT32_MAX;
        for(uint32_t i=0;i<(uint32_t)sb->data_region_blocks; i++){
            if(!test_bit(data_bmap, i)){ free_idx = i; break; }
        }
        if(free_idx==UINT32_MAX){
            fprintf(stderr,"No free data blocks to extend root directory\n");
            free(db_idxs); free(img); return 7;
        }
        set_bit(data_bmap, free_idx);
        uint32_t abs = (uint32_t)(sb->data_region_start + free_idx);
        root->direct[slot] = abs;
        uint8_t* blk = img + BS*abs;
        memset(blk, 0, BS);

        dirent64_t ne; memset(&ne,0,sizeof(ne));
        ne.inode_no = new_ino_no;
        ne.type = 1;
        memcpy(ne.name, namebuf, namelen);
        dirent_checksum_finalize(&ne);
        memcpy(blk, &ne, sizeof(ne));

        root->size_bytes += sizeof(dirent64_t);
        root->mtime = root->ctime = now;
        root->links += 1;
        inode_crc_finalize(root);
        placed = 1;
    }

    // Write output image
    FILE* fo = fopen(cli.out_img, "wb");
    if(!fo){ perror("fopen output"); free(db_idxs); free(img); return 1; }
    size_t blocks_written = fwrite(img, BS, (size_t)total_blocks, fo);
    fclose(fo);
    free(db_idxs); free(img);
    if(blocks_written != total_blocks){
        fprintf(stderr,"Short write on output image\n"); return 1;
    }
    fprintf(stdout,"Added '%s' (inode #%u) into '%s' -> '%s'\n",
            base, new_ino_no, cli.in_img, cli.out_img);
    return 0;
}
