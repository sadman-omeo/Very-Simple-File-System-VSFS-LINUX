// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder_skeleton.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define SUPERBLOCK_MAGIC 0x53465356696e694d

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

#pragma pack(push, 1)
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t num_blocks;
    uint32_t num_inodes;
    uint32_t num_free_blocks;
    uint32_t num_free_inodes;
    uint32_t inode_bitmap_block;
    uint32_t data_bitmap_block;
    uint32_t inode_table_block;
    uint32_t data_block_start;
    uint8_t padding[4044];
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == BS, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t direct_blocks[12];
    uint32_t indirect_block;
    uint32_t double_indirect_block;
    uint8_t padding[24];
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t ino;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

int main(int argc, char *argv[]) {
    crc32_init();

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output_file>\n", argv[0]);
        return 1;
    }

    char *output_file = argv[1];

    uint32_t num_blocks = 1024;
    uint32_t num_inodes = 128;
    uint32_t inode_bitmap_blocks = 1;
    uint32_t data_bitmap_blocks = 1;
    uint32_t inode_table_blocks = (num_inodes * INODE_SIZE) / BS;
    if ((num_inodes * INODE_SIZE) % BS != 0) {
        inode_table_blocks++;
    }


    FILE *fp = fopen(output_file, "wb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = SUPERBLOCK_MAGIC;
    sb.version = 1;
    sb.block_size = BS;
    sb.num_blocks = num_blocks;
    sb.num_inodes = num_inodes;
    sb.inode_bitmap_block = 1;
    sb.data_bitmap_block = sb.inode_bitmap_block + inode_bitmap_blocks;
    sb.inode_table_block = sb.data_bitmap_block + data_bitmap_blocks;
    sb.data_block_start = sb.inode_table_block + inode_table_blocks;
    sb.num_free_blocks = num_blocks - sb.data_block_start;
    sb.num_free_inodes = num_inodes - 1;

    superblock_crc_finalize(&sb);
    fwrite(&sb, sizeof(sb), 1, fp);

    uint8_t *inode_bitmap = calloc(inode_bitmap_blocks, BS);
    inode_bitmap[0] = 0b00000011; // Mark first two inodes as used (0 is unused, 1 is root)
    fwrite(inode_bitmap, BS, inode_bitmap_blocks, fp);
    free(inode_bitmap);

    uint8_t *data_bitmap = calloc(data_bitmap_blocks, BS);
    data_bitmap[0] = 0b00000001; // Mark first data block as used for root directory
    fwrite(data_bitmap, BS, data_bitmap_blocks, fp);
    free(data_bitmap);

    inode_t *inode_table = calloc(inode_table_blocks, BS);
    inode_t *root_inode = &inode_table[ROOT_INO];
    root_inode->mode = 040755; // Directory with rwxr-xr-x
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size = BS;
    root_inode->atime = time(NULL);
    root_inode->mtime = time(NULL);
    root_inode->ctime = time(NULL);
    root_inode->links_count = 2;
    root_inode->blocks = 1;
    root_inode->direct_blocks[0] = sb.data_block_start;
    inode_crc_finalize(root_inode);
    fwrite(inode_table, BS, inode_table_blocks, fp);
    free(inode_table);

    dirent64_t *root_dir_entries = calloc(1, BS);

    dirent64_t *dot_entry = &root_dir_entries[0];
    dot_entry->ino = ROOT_INO;
    dot_entry->type = 2; // Directory
    strcpy(dot_entry->name, ".");
    dirent_checksum_finalize(dot_entry);

    dirent64_t *dotdot_entry = &root_dir_entries[1];
    dotdot_entry->ino = ROOT_INO;
    dotdot_entry->type = 2; // Directory
    strcpy(dotdot_entry->name, "..");
    dirent_checksum_finalize(dotdot_entry);

    fseek(fp, sb.data_block_start * BS, SEEK_SET);
    fwrite(root_dir_entries, BS, 1, fp);
    free(root_dir_entries);

    fclose(fp);

    printf("File system image '%s' created successfully.\n", output_file);

    return 0;
}
