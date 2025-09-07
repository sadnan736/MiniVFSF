// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
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

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

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
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
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
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}
static inline void lsb_btmap_set(uint8_t *bitmap, uint32_t index){
    uint32_t byte_index = index / 8; 

    uint32_t bit_position = index % 8; 

    uint8_t mask = 1;
    for (uint32_t i = 0; i < bit_position; i++) {
        mask *= 2; 
    }

    // set the bit: "turn it on" if not already
    bitmap[byte_index] = bitmap[byte_index] + ( (bitmap[byte_index] & mask) ? 0 : mask );
    
}

static void build_directory_entry(dirent64_t *entry, uint32_t ino, uint8_t kind, const char *name) {
    *entry = (dirent64_t){0};
    entry->inode_no = ino;
    entry->type = kind;
    size_t cap = sizeof entry->name; // 58
    size_t i = 0;
    if (name) { 
        while (i < cap && name[i] != '\0') {
            entry->name[i] = name[i]; i++; 
        } 
    } 
    dirent_checksum_finalize(entry);  
}
int main(int argc, char *argv[]) {
    crc32_init();
    if (argc != 7){

        printf("Please enter the correct parameteres");
        return 0;
    }
    char *out_img = NULL;
    uint64_t size_kib=0, inode_count=0;
    int i = 0;

    while (i < argc){
        if (!strcmp(argv[i],"--image")){
            out_img=argv[++i];
            
        } else if (!strcmp(argv[i],"--size-kib")){ 
            size_kib=strtoull(argv[++i],NULL,10);
            
        } else if (!strcmp(argv[i],"--inodes")){
            inode_count=strtoull(argv[++i],NULL,10);
            
        }

        i++;
        
    }

    if ((!(size_kib>=180 && size_kib<=4096) || (size_kib%4)!=0)||
        !out_img ||
        !(inode_count>=128 && inode_count<= 512)){
            fprintf(stderr,
        "Parameters:\n"
        "  --image     = %s\n"
        "  --size-kib  = %" PRIu64 "\n"
        "  --inodes    = %" PRIu64 "\n",
        out_img ? out_img : "(null)", size_kib, inode_count);

        printf("Please enter the correct parameteres");
        return 0;
    }

    //superblock

    uint8_t super_bl_ock[BS]; memset(super_bl_ock,0,BS);
    superblock_t *sb = (superblock_t*)super_bl_ock;
    
    sb->magic = 0x4D565346;
    sb->version = 1;
    sb->block_size = BS;
	sb->total_blocks = (size_kib*1024)/BS;
	sb->inode_count =inode_count;
	sb->inode_bitmap_start = 1;
	sb->inode_bitmap_blocks = 1;
	sb->data_bitmap_start = 2;
	sb->data_bitmap_blocks = 1;
	sb->inode_table_start = 3;
	sb->inode_table_blocks = (inode_count*INODE_SIZE + BS-1)/BS;
	sb->data_region_start = sb->inode_table_start + sb->inode_table_blocks;
	sb->data_region_blocks = sb->total_blocks-sb->data_region_start;
	sb->root_inode = 1;
	sb->mtime_epoch = time(NULL);
    superblock_crc_finalize(sb);

    // inode table
    uint8_t *inode_table = calloc(sb->inode_table_blocks,BS);

    inode_t *for_root = (inode_t*)inode_table;
    memset(for_root,0,sizeof(*for_root));

    for_root->mode=0040000;
    for_root->links=2;
    for_root->size_bytes = 64*2;
    for_root->atime=for_root->mtime=for_root->ctime=sb->mtime_epoch;
    for_root->direct[0]=sb->data_region_start;
    for_root->direct[0]=sb->data_region_start;
    inode_crc_finalize(for_root);

    /* bitmaps */
    uint8_t ibm[BS]; memset(ibm, 0, sizeof ibm);
    lsb_btmap_set(ibm, 0);               /* root inode */

    uint8_t dbm[BS]; memset(dbm, 0, sizeof dbm);
    lsb_btmap_set(dbm, 0);               /* root dir block */

    /* root dir block */
    uint8_t *rootblk = (uint8_t*)calloc(1, BS);
    dirent64_t *e_dot    = (dirent64_t*)rootblk;
    dirent64_t *e_dotdot = (dirent64_t*)(rootblk + sizeof(dirent64_t));

    build_directory_entry(e_dot,    1, 2, ".");
    build_directory_entry(e_dotdot, 1, 2, "..");

    /* write image */
    FILE *fp = fopen(out_img, "wb");
    if(!fp){ perror("fopen"); return 1; }

    fwrite(super_bl_ock, 1, BS, fp);
    fwrite(ibm,          1, BS, fp);
    fwrite(dbm,          1, BS, fp);
    fwrite(inode_table,  BS, sb->inode_table_blocks, fp);
    fwrite(rootblk,      1, BS, fp);

    // pad tail 
    uint64_t used = 1 /*sb*/ + 1 /*ibm*/ + 1 /*dbm*/ + sb->inode_table_blocks + 1 /*root*/;
    uint64_t left = sb->total_blocks - used;

    uint8_t *zeros = (uint8_t*)calloc(1, BS);
    for (uint64_t k = 0; k < left; k++) {
        fwrite(zeros, 1, BS, fp);
    }

    fclose(fp);
    free(rootblk);
    free(zeros);


    printf("Created .img FIle:\n'%s'\nTotal Blocks: %llu blocks\nTotal Nodes: %llu inodes\nInode Table Block: %llu\n",
           out_img,(unsigned long long)sb->total_blocks,(unsigned long long)inode_count, (unsigned long long) sb->inode_table_blocks);
    return 0;

}
