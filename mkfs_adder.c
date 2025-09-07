#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h> 
#include <time.h> 
#include <errno.h>
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
    uint32_t checksum;            // crc32(superblock[0..4091])
}  superblock_t;
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
static inline int lsb_btmap_test(uint8_t *bm, uint32_t idx) {
    uint32_t byte_index = idx / 8;
    uint32_t bit_index  = idx % 8;

    uint8_t mask = 1;
    for (uint32_t i = 0; i < bit_index; i++) {
        mask *= 2;
    }
    if (bm[byte_index] / mask % 2 == 1) {
        return 1;
    } else {
        return 0;
    }
}

static inline uint32_t find_first_free_bit(uint8_t *bm, uint32_t limit) {
    for (uint32_t i = 0; i < limit; i++) {
        if (!lsb_btmap_test(bm, i)) return i;
    }
    return UINT32_MAX;
}


static inline uint32_t allocate_inode_index(uint8_t *inode_bmp, const superblock_t *sb) {
    uint32_t idx = find_first_free_bit(inode_bmp, (uint32_t)sb->inode_count);
    if (idx != UINT32_MAX) lsb_btmap_set(inode_bmp, idx);
    return idx;
}

//  Absolute = data_region_start + Relative = 25
static inline uint32_t allocate_block_abs_index(uint8_t *data_bmp, const superblock_t *sb) {
    uint32_t rel = find_first_free_bit(data_bmp, (uint32_t)sb->data_region_blocks);
    if (rel == UINT32_MAX) return UINT32_MAX;
    lsb_btmap_set(data_bmp, rel);
    return (uint32_t)sb->data_region_start + rel;
}






int main(int argc, char *argv[]) {
    crc32_init();
    if (argc != 7){

        fprintf(stderr, "Usage: %s --input in.img --output out.img --file filename\n", argv[0]);
        return 1;

    }
    char *input = NULL,*output = NULL,*filename = NULL;

    int i = 1;

    while(i<argc){
        if (!strcmp(argv[i],"--input")){
            input=argv[++i];
        } else if (!strcmp(argv[i],"--output")){
            output=argv[++i];
        } else if (!strcmp(argv[i],"--file")){
            filename=argv[++i];
        }

        i++;
    }
    if (!input || !output || !filename){
        printf("Please enter the correct parameteres");
        return 1;
    }
    if (strcmp(filename, ".")==0 || strcmp(filename, "..")==0) {
        fprintf(stderr,"--file cannot be '.' or '..'\n");
        return 1;
    }

    if (strlen(filename) > 58) {
        fprintf(stderr,"--file name too long (max 58)\n");
        return 1;
    }

    struct stat st;   // <- this creates the variable
    if (stat(filename, &st) != 0) {
        perror("stat");
        return 1;
    }
if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "'%s' is not a regular file\n", filename);
    return 1;
    }



    FILE *fs=fopen(input,"rb");
    if (!fs) { fprintf(stderr,"open '%s': %s\n", input, strerror(errno)); return 1; }
    if (fseek(fs,0,SEEK_END)!=0) { fprintf(stderr,"seek '%s': %s\n", input, strerror(errno)); fclose(fs); return 1; }
    long img_sz_long = ftell(fs);
    if (img_sz_long < 0) { fprintf(stderr,"tell '%s': %s\n", input, strerror(errno)); fclose(fs); return 1; }
    rewind(fs);
    size_t img_sz = (size_t)img_sz_long;
    uint8_t *img = (uint8_t*)malloc(img_sz);
    if (!img) { fprintf(stderr,"malloc img: %s\n", strerror(errno)); fclose(fs); return 1; }
    if (fread(img,1,img_sz,fs) != img_sz) { fprintf(stderr,"read '%s': %s\n", input, strerror(errno)); fclose(fs); free(img); return 1; }
    fclose(fs);



    // map structs 
    superblock_t *sb=(superblock_t*)img;    
    uint8_t *index_of_node_bmp = img + sb->inode_bitmap_start*BS; 
    uint8_t *data_bmp = img + sb->data_bitmap_start*BS;  
    inode_t *index_of_node_table = (inode_t*)(img +      sb->inode_table_start*BS);

    if (sb->magic != 0x4D565346u || sb->block_size != BS) {
    fprintf(stderr,"bad superblock\n"); free(img); return 1;
}



    // text file
    FILE *read_text_file = fopen(filename,"rb");
    if (!read_text_file) { fprintf(stderr,"open '%s': %s\n", filename, strerror(errno)); free(img); return 1; }
    if (fseek(read_text_file,0,SEEK_END)!=0) { fprintf(stderr,"seek '%s': %s\n", filename, strerror(errno)); fclose(read_text_file); free(img); return 1; }
    long txt_size_long = ftell(read_text_file);
    if (txt_size_long < 0) { fprintf(stderr,"tell '%s': %s\n", filename, strerror(errno)); fclose(read_text_file); free(img); return 1; }
    rewind(read_text_file);
    size_t txt_size = (size_t)txt_size_long;

    uint8_t *text_fie = (uint8_t*)malloc(txt_size ? txt_size : 1);
    if (!text_fie) { fprintf(stderr,"malloc file: %s\n", strerror(errno)); fclose(read_text_file); free(img); return 1; }
    if (txt_size && fread(text_fie,1,txt_size,read_text_file) != txt_size) {
        fprintf(stderr,"read '%s': %s\n", filename, strerror(errno));
        fclose(read_text_file); free(text_fie); free(img); return 1;
    }
    fclose(read_text_file);


    if (txt_size > DIRECT_MAX*BS) { fprintf(stderr,"file too large\n"); free(text_fie); free(img); return 1; }

    size_t name_len = strlen(filename);
    if (name_len > 58) {
        fprintf(stderr,"name too long (58 max)\n");
        free(text_fie); free(img); return 1;
    }

    // allocate innode
    uint32_t innode_index = allocate_inode_index(index_of_node_bmp, sb);

    if (innode_index == UINT32_MAX) {
        fprintf(stderr,"No free inode\n");
        return 0;
    }

    inode_t *inode = &index_of_node_table[innode_index];

    // allocate data block
    uint32_t need = (txt_size + BS - 1) / BS;


    for (uint32_t i = 0; i < need; i++) {
    uint32_t abs = allocate_block_abs_index(data_bmp, sb);
    if (abs == UINT32_MAX) { fprintf(stderr,"no free data block\n"); free(text_fie); free(img); return 1; }
    inode->direct[i] = abs;

    size_t off  = (size_t)i * BS;
    size_t left = txt_size - off;
    size_t n    = left < BS ? left : BS;
    memcpy(img + (size_t)abs * BS, text_fie + off, n);
    if (n < BS) memset(img + (size_t)abs * BS + n, 0, BS - n);
}

    free(text_fie);

    // write text on inode
    memset(inode,0,sizeof(*inode));
    inode->mode=0100000;
    inode->links=1;
    inode->size_bytes=txt_size;
    inode->atime=inode->mtime=inode->ctime=time(NULL);
    inode_crc_finalize(inode);

    // add file.txt to root dir infromation
    inode_t *root=&index_of_node_table[0];
    dirent64_t *ents=(dirent64_t*)(img+root->direct[0]*BS);
    for (int i=0;i<BS/64;i++) {
        if (ents[i].inode_no==0) {
            build_directory_entry(&ents[i],innode_index+1,1,filename);
            break;
        }
    }
    root->size_bytes+=64;
    inode_crc_finalize(root);

    //update sb

    sb->mtime_epoch=time(NULL);
    superblock_crc_finalize(sb);

    // write back

    FILE *fo = fopen(output,"wb");
    if (!fo) { fprintf(stderr,"open '%s': %s\n", output, strerror(errno)); free(text_fie); free(img); return 1; }
    if (fwrite(img,1,img_sz,fo) != img_sz) { fprintf(stderr,"write '%s': %s\n", output, strerror(errno)); fclose(fo); free(text_fie); free(img); return 1; }
    if (fflush(fo)!=0) { fprintf(stderr,"fflush '%s': %s\n", output, strerror(errno)); }
    fclose(fo);
    
    free(img);

    printf("Image Updated: '%s'\n Added:'%s'\nNew File: '%s'\n",input,filename,output);

}
