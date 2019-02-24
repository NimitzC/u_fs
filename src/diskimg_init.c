/**
 * A format program to init diskimg.
 * i.e. write its super block and bitmap blocks data.
 * 
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#define BLOCK_SIZE 512
#define NUM_SUPER_BLOCK 1
#define NUM_BITMAP_BLOCK 1280
#define MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long) - sizeof(size_t))
#define MAX_FILENAME 8
#define MAX_EXTENSION 3
#define NO_NEXT -1

#define BIT_TO_BYTE(b) ((b)/8)
typedef unsigned char BYTE;

static size_t get_file_size(const char* filepath);
static void print_binary(BYTE byte, int size); //将n二进制输出

struct sb {//24bytes
    long fs_size; //size of file system, in blocks
    long first_blk; //first block of root directory
    long bitmap; //size of bitmap, in blocks
};

struct u_fs_file_directory { //40bytes
    char fname[MAX_FILENAME + 1]; //filename (plus space for nul)
    char fext[MAX_EXTENSION + 1]; //extension (plus space for nul)
    size_t fsize; //file size
    long nStartBlock; //where the first block is on disk
    int flag; //indicate type of file. 0:for unused; 1:for file; 2:for directory
};

struct u_fs_disk_block { //512bytes
    size_t size; // how many bytes are being used in this block
    long nNextBlock; //The next disk block, if needed. 
                     //This is the next pointer in the linked allocation list
    char data[MAX_DATA_IN_BLOCK]; //And all the rest of the space in the block...
                                  //can be used for actual data storage.
};

int main()
{

    // the path should be like /home/zzy/Desktop/OS/.../diskimg
    const char* diskimg_path = "/home/zzy/Desktop/OS/diskimg";
    if(access(diskimg_path, F_OK) != 0) { //diskimg_path is not existed
        printf("diskimg path is not existed\n");
        return -1;
    }
    const size_t diskimg_size = get_file_size(diskimg_path);

    /**
     * 1. init super block
     */
    FILE *fp;
    fp = fopen(diskimg_path, "r+");
    if(fp == NULL){
        perror("diskimg open failed\n");
        return 3;
    }
    struct sb *sblk = malloc(sizeof(struct sb)); 
    sblk->fs_size = diskimg_size / BLOCK_SIZE;
    sblk->first_blk = NUM_SUPER_BLOCK + NUM_BITMAP_BLOCK; //1 + 1280 = 1281
    sblk->bitmap = NUM_BITMAP_BLOCK;

    if(fseek(fp, 0, SEEK_SET) !=0){
        perror("init super block fseek error\n");
        return 4;
    }
    fwrite(sblk, sizeof(struct sb), 1, fp);
    free(sblk);
    sblk = NULL;
    printf("super block format finished\n");

    /**
     * 2. init bitmap block
     * the total bits of bitmap = diskimg_size / BLOCK_SIZE
     */
    BYTE byte[NUM_BITMAP_BLOCK / 8];
    memset(byte, -1, NUM_BITMAP_BLOCK / 8);
    BYTE s_byte = (3<<6);
    BYTE rest_byte[(diskimg_size / BLOCK_SIZE) - NUM_BITMAP_BLOCK - 8];
    if(fseek(fp, BLOCK_SIZE * 1, SEEK_SET) != 0){
        perror("init bitmap block fseek error\n");
        return 5;
    }
    fwrite(byte, sizeof(byte), 1, fp);
    fwrite(&s_byte, sizeof(s_byte), 1, fp);
    fwrite(rest_byte, sizeof(rest_byte), 1, fp);
    printf("bitmap block format finished\n");

    /**
     * 3. init first data block
     * this is for root directory block
     */
    struct u_fs_disk_block *root = malloc(sizeof(struct u_fs_disk_block));
	root->size = 0;
	root->nNextBlock = -1;
	root->data[0] = '\0';
    if(fseek(fp, BLOCK_SIZE * (NUM_BITMAP_BLOCK + 1), SEEK_SET) != 0){
        perror("init first data block fseek error\n");
        return 6;
    }
    fwrite(root, sizeof(struct u_fs_disk_block), 1, fp);
    free(root);
    root = NULL;
    printf("first data block format finished\n");

    if(fclose(fp) != 0){
        perror("file closed failed\n");
    }

    printf("format finished!\n");
    return 0;
}

static size_t get_file_size(const char* filepath){
    struct stat statbuf;
    stat(filepath, &statbuf);
    size_t size = statbuf.st_size;
    return size;
}

static void print_binary(BYTE n, int size){
    if(size == 0) return;
    print_binary((n>>1), (size-1));
    printf("%d", n&1);
}
