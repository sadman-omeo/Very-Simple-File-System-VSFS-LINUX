#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define SUPERBLOCK_MAGIC 0x53465356696e694d

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

// Function to find a free inode
int find_free_inode(uint8_t *inode_bitmap, uint32_t num_inodes) {
    for (uint32_t i = 0; i < num_inodes; i++) {
        if (!((inode_bitmap[i / 8] >> (i % 8)) & 1)) {
            return i;
        }
    }
    return -1;
}

// Function to find a free data block
int find_free_block(uint8_t *data_bitmap, uint32_t num_blocks) {
    for (uint32_t i = 0; i < num_blocks; i++) {
        if (!((data_bitmap[i / 8] >> (i % 8)) & 1)) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char *argv[]) {
    crc32_init();

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <image_file> <file_to_add>\n", argv[0]);
        return 1;
    }

    char *image_file = argv[1];
    char *file_to_add_path = argv[2];

    FILE *fp_img = fopen(image_file, "r+b");
    if (!fp_img) {
        perror("fopen image_file");
        return 1;
    }

    FILE *fp_add = fopen(file_to_add_path, "rb");
    if (!fp_add) {
        perror("fopen file_to_add");
        fclose(fp_img);
        return 1;
    }

    fseek(fp_add, 0, SEEK_END);
    long file_size = ftell(fp_add);
    fseek(fp_add, 0, SEEK_SET);

    char *file_content = malloc(file_size);
    if(!file_content){
        fprintf(stderr, "Failed to allocate memory for file content\n");
        fclose(fp_img);
        fclose(fp_add);
        return 1;
    }
    fread(file_content, file_size, 1, fp_add);
    fclose(fp_add);


    superblock_t sb;
    fread(&sb, sizeof(sb), 1, fp_img);

    if (sb.magic != SUPERBLOCK_MAGIC) {
        fprintf(stderr, "Invalid filesystem magic number\n");
        fclose(fp_img);
        free(file_content);
        return 1;
    }

    uint32_t num_data_blocks_needed = (file_size + BS - 1) / BS;
    if(num_data_blocks_needed > 12){
        fprintf(stderr, "File too large for direct blocks only. Indirect blocks not implemented in this adder.\n");
        fclose(fp_img);
        free(file_content);
        return 1;
    }


    uint8_t *inode_bitmap = malloc(BS);
    fseek(fp_img, sb.inode_bitmap_block * BS, SEEK_SET);
    fread(inode_bitmap, BS, 1, fp_img);

    int free_inode_idx = find_free_inode(inode_bitmap, sb.num_inodes);
    if (free_inode_idx == -1) {
        fprintf(stderr, "No free inodes available\n");
        fclose(fp_img);
        free(inode_bitmap);
        free(file_content);
        return 1;
    }
    inode_bitmap[free_inode_idx / 8] |= (1 << (free_inode_idx % 8));


    uint8_t *data_bitmap = malloc(BS);
    fseek(fp_img, sb.data_bitmap_block * BS, SEEK_SET);
    fread(data_bitmap, BS, 1, fp_img);

    uint32_t *allocated_blocks = malloc(num_data_blocks_needed * sizeof(uint32_t));
    for (uint32_t i = 0; i < num_data_blocks_needed; i++) {
        int free_block_idx = find_free_block(data_bitmap, sb.num_blocks);
        if (free_block_idx == -1) {
            fprintf(stderr, "Not enough free data blocks\n");
            // Rollback inode bitmap change
            inode_bitmap[free_inode_idx / 8] &= ~(1 << (free_inode_idx % 8));
            fclose(fp_img);
            free(inode_bitmap);
            free(data_bitmap);
            free(allocated_blocks);
            free(file_content);
            return 1;
        }
        data_bitmap[free_block_idx / 8] |= (1 << (free_block_idx % 8));
        allocated_blocks[i] = sb.data_block_start + free_block_idx;
        
        fseek(fp_img, allocated_blocks[i] * BS, SEEK_SET);
        fwrite(file_content + (i * BS), (i == num_data_blocks_needed - 1) ? (file_size % BS == 0 ? BS : file_size % BS) : BS, 1, fp_img);
    }
    free(file_content);


    inode_t *inode_table = malloc(sb.inode_table_block * BS);
    fseek(fp_img, sb.inode_table_block * BS, SEEK_SET);
    fread(inode_table, sb.inode_table_block * BS, 1, fp_img);

    inode_t *new_inode = &inode_table[free_inode_idx];
    new_inode->mode = 0100644; // Regular file rw-r--r--
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size = file_size;
    new_inode->atime = time(NULL);
    new_inode->mtime = time(NULL);
    new_inode->ctime = time(NULL);
    new_inode->links_count = 1;
    new_inode->blocks = num_data_blocks_needed;
    for(uint32_t i = 0; i < num_data_blocks_needed; ++i) {
        new_inode->direct_blocks[i] = allocated_blocks[i];
    }
    inode_crc_finalize(new_inode);


    inode_t *root_inode = &inode_table[ROOT_INO];
    dirent64_t *root_dir = malloc(BS);
    fseek(fp_img, root_inode->direct_blocks[0] * BS, SEEK_SET);
    fread(root_dir, BS, 1, fp_img);

    int dirent_added = 0;
    for (int i = 0; i < (BS / sizeof(dirent64_t)); i++) {
        if (root_dir[i].ino == 0) { // Find an empty dirent 
            root_dir[i].ino = free_inode_idx;
            root_dir[i].type = 1; // File
            strncpy(root_dir[i].name, file_to_add_path, sizeof(root_dir[i].name) - 1);
            root_dir[i].name[sizeof(root_dir[i].name) - 1] = '\0';
            dirent_checksum_finalize(&root_dir[i]);
            dirent_added = 1;
            break;
        }
    }

    if(!dirent_added){
        fprintf(stderr, "No space in root directory for new file\n");
        // A more robust implementation would handle this, maybe by allocating a new block for the directory
        // For now, we just error out and should ideally roll back all changes.
        fclose(fp_img);
        free(inode_bitmap);
        free(data_bitmap);
        free(allocated_blocks);
        free(inode_table);
        free(root_dir);
        return 1;
    }
    
    root_inode->mtime = time(NULL);
    inode_crc_finalize(root_inode);


    sb.num_free_inodes--;
    sb.num_free_blocks -= num_data_blocks_needed;
    superblock_crc_finalize(&sb);

    fseek(fp_img, 0, SEEK_SET);
    fwrite(&sb, sizeof(sb), 1, fp_img);

    fseek(fp_img, sb.inode_bitmap_block * BS, SEEK_SET);
    fwrite(inode_bitmap, BS, 1, fp_img);
    free(inode_bitmap);

    fseek(fp_img, sb.data_bitmap_block * BS, SEEK_SET);
    fwrite(data_bitmap, BS, 1, fp_img);
    free(data_bitmap);

    fseek(fp_img, sb.inode_table_block * BS, SEEK_SET);
    fwrite(inode_table, sb.inode_table_block * BS, 1, fp_img);
    free(inode_table);

    fseek(fp_img, root_inode->direct_blocks[0] * BS, SEEK_SET);
    fwrite(root_dir, BS, 1, fp_img);
    free(root_dir);

    fclose(fp_img);
    free(allocated_blocks);

    printf("File '%s' added to '%s' successfully.\n", file_to_add_path, image_file);

    return 0;
}
