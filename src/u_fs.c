/** @file
 *
 * Operting System final project based on FUSE 3.3.0
 * u_fs重构
 *
 * Compile with
 *
 *     gcc -Wall u_fs.c `pkg-config fuse3 --cflags --libs` -o u_fs
 *
 * ## Source code ##
 * \include u_fs.c
 */

/**
 * 规定：
 * 目录名不能有后缀
 * 文件名可以起后缀
 * 文件和目录不能同名（没有后缀的情况）
 */

/**
 * 注意：
 * 在使用前确定diskimg（虚拟磁盘文件)的路径是否正确
 * 见下方的全局变量 DISKIMG_PATH，如果位置有更改，请修改
 * 为你对应的diskimg的位置
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>

#define BLOCK_SIZE 512
#define NUM_SUPER_BLOCK 1
#define NUM_BITMAP_BLOCK 1280
#define MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long) - sizeof(size_t))
#define MAX_FILENAME 8
#define MAX_EXTENSION 3

long NUM_TOTAL_BLOCK; //在u_fs_init中进行初始化
typedef unsigned char BYTE;
const char *DISKIMG_PATH = "/home/zzy/Desktop/OS/diskimg";

struct sb { //24bytes
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

static void *u_fs_init(struct fuse_conn_info *conn, struct fuse_config *cfg);
static int u_fs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
static int u_fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
static int u_fs_mkdir(const char *path, mode_t mode);
static int u_fs_rmdir(const char *path);
static int u_fs_mknod(const char *path, mode_t mode, dev_t rdev);
static int u_fs_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi);
static int u_fs_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi);
static int u_fs_unlink(const char *path);
static int u_fs_open(const char *path, struct fuse_file_info *fi);
static int u_fs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
static int u_fs_flush(const char *path, struct fuse_file_info *fi);

static struct fuse_operations u_fs_oper = {
	.init = u_fs_init,
	.getattr = u_fs_getattr,
	.readdir = u_fs_readdir,
	.mkdir = u_fs_mkdir,
	.rmdir = u_fs_rmdir,
	.mknod = u_fs_mknod,
	.read = u_fs_read,
	.write = u_fs_write,
	.unlink = u_fs_unlink,
    .truncate = u_fs_truncate,
    .open = u_fs_open,
    .flush = u_fs_flush
};

/** enlarge_a_block()
 * 功能：给disk_blk扩充一个块，返回扩充新块的块号
 * 参数：n_blk：需要扩充的块号; disk_blk：一个申请好内存空间的u_fs_disk_block类型的指针
 * 返回：-1 失败; 成功则返回新块的块号
 */
static long enlarge_a_block(const long n_blk, struct u_fs_disk_block * const disk_blk);

/** clear blocks()
 * 功能：释放包括start_blk开始的后续块，同时在位图中将对应位的占用改为空闲
 * 参数：star_blk：起始块块号
 * 返回：-1 失败; 0 成功 
 */
static int clear_blocks(const long start_blk);

/** strcnt()
 * 功能：在str字符串中统计ch字符的次数
 * 参数：str：字符串; ch：被统计字符 
 * 返回：统计结果，没找到即返回0
 */
static int strcnt(const char* str, const char ch);

/** read_disk_block()
 * 功能：在diskimg中读出一个块的，保存在disk_blk中
 * 参数：n_blk：需要读的块号; disk_blk：一个申请好内存空间的u_fs_disk_block类型的指针
 * 返回：-1 失败; 0 成功
 */
static int read_disk_block(long n_blk, struct u_fs_disk_block *disk_blk);

/** write_disk_block()
 * 功能：往diskimg中写一个块的内容
 * 参数：n_blk：需要写的块号; disk_blk：需要写往diskimg的内容
 * 返回：-1 失败; 0 成功
 */
static int write_disk_block(long n_blk, struct u_fs_disk_block *disk_blk);

/** set_single_bit_in_bitmap()
 * 功能：在diskimg的位图块中设置指定位为0或1
 * 参数：n_blk：需要设置的块号; flag：置为0还是1
 * 返回：-1 失败; 0 成功
 */
static int set_single_bit_in_bitmap(const long n_blk, const int flag);

/** get_consecutive_free_blocks()
 * 功能：获取长度为n_blk的连续空闲块，并置位图中相应位置为占用（即1的情况）
 * 参数：n_blk：需要设置的块号; flag：置为0还是1
 * 返回：-1 成功; -2 出错; >=0 返回剩余空闲块总数
 */
static int get_consecutive_free_blocks(const long n_blk, long* start_blk);

/** check_path()
 * 功能：检查传入的路径path是否正确（纯粹的字符串检查），并分割路径
 * 参数：path：路径; dirname：子目录名（只有路径是子目录下的文件时才不会赋""值）
 * 参数：fname: 文件名; fext: 后缀名
 * 返回：-2 有文件名/目录名过长; -1 路径错误; 0 路径是根目录; 
 * 返回：1 根目录下，不带后缀名的文件或目录; 2 子目录下的文件
 */
static int check_path(const char * const path, char * const dirname,
                    char * const fname, char * const fext);

/** read_stat_in_rootdir()
 * 功能：传入文件/目录项的名称，读取根目录下对应的项的所有属性
 * 参数：fname：文件/目录名; fext：后缀名（没有时请传入""）
 * 参数：f_dir: 分配好内存的u_fs_file_directory指针，得到属性会被保存在这里
 * 返回：-1 失败; 成功则返回该项处在根目录的哪个块上; 
 */
static long read_stat_in_rootdir(const char* const fname, const char * const fext, 
                                struct u_fs_file_directory* f_dir);

/** read_stat_from_block()
 * 功能：传入文件/目录项的名称，读取指定目录块中下对应的项的所有属性
 * 参数：fname：文件/目录名; fext：后缀名（没有时请传入""）; blk：指定目录块号
 * 参数：f_dir: 分配好内存的u_fs_file_directory指针，得到属性会被保存在这里
 * 返回：-1 失败; 成功则返回该项处在指定目录的哪个块上; 
 */
static long read_stat_from_block(const char* const fname, const char* const fext, 
                                        const long blk, struct u_fs_file_directory* f_dir);

/** read_stat_from_path()
 * 功能：传入路径，找到并读出对应的项的所有属性
 * 参数：path：路径;
 * 参数：f_dir: 分配好内存的u_fs_file_directory指针，得到属性会被保存在这里
 * 返回：-2 名词过长; -1 失败; 0 根目录; >0该项处在指定目录的哪个块上; 
 */
static long read_stat_from_path(const char* path, struct u_fs_file_directory* f_dir);

/** write_stat_from_block()
 * 功能：写回属性到指定目录块
 * 参数：blk：属性需要写到哪个块上;
 * 参数：f_dir: 分配好内存的u_fs_file_directory指针，得到属性会被保存在这里
 * 返回：-1 失败; 0 成功; 
 */
static int write_stat_from_block(const long blk, struct u_fs_file_directory const * const f_dir);

/** cp_item()
 * 功能：将src的文件/目录项属性复制到dest上
 * 参数：src：源内容; dest: 目的内容
 * 返回：NULL
 */
static void cp_item(struct u_fs_file_directory * const dest, struct u_fs_file_directory const * const src);

/** reset_item()
 * 功能：将dest的内容设为默认值
 * 参数：dest: 目的内容
 * 返回：NULL
 */
static void reset_item(struct u_fs_file_directory * const dest);

/** move_to_last_item()
 * 功能：移动it指针指向db块的的最后记录的位置
 * 参数：it：需要修改指针的指向; db：disk_block块
 * 返回：NULL
 */
static void move_to_last_item(struct u_fs_file_directory **it, struct u_fs_disk_block const * const db);

/** rm_item()
 * 功能：从指定块中删除一个文件/目录项，采取了回填策略，同时置位位图
 * 参数：i_blk：哪个块中的项目; f_dir：需要删除项目的一切属性
 * 返回：-1 失败; 0 成功
 */
static int rm_item(const long i_blk, struct u_fs_file_directory const * const f_dir);

int main(int argc, char *argv[])
{
	umask(0);
	return fuse_main(argc, argv, &u_fs_oper, NULL);
}

static int read_disk_block(long num_block, struct u_fs_disk_block *disk_block){
	FILE *fp;
	fp = fopen(DISKIMG_PATH, "r+");
	if (fp == NULL){
		perror("read_disk_block_info(): open file failed");
		return -1;
	}
	if (fseek(fp, num_block * BLOCK_SIZE, SEEK_SET) != 0){
		perror("read_disk_block_info(): fseek error");
		fclose(fp);
		return -1;
	}
	fread(disk_block, sizeof(struct u_fs_disk_block), 1, fp);
	if (ferror(fp) || feof(fp)) {
		fclose(fp);
		perror("read_disk_block_info(): read file wrong");
		return -1;
	}
	fclose(fp);
	return 0;
}

static int write_disk_block(long num_block, struct u_fs_disk_block *disk_block){
	FILE* fp;
	fp = fopen(DISKIMG_PATH, "r+");
	if (fp == NULL){
		perror("write_disk_block_info(): open file failed");
		return -1;
	}	
	if (fseek(fp, num_block * BLOCK_SIZE, SEEK_SET) != 0) {
		perror("write_disk_block_info(): fseek error");
		fclose(fp);
		return -1;
	}
	fwrite(disk_block, sizeof(struct u_fs_disk_block), 1, fp);
	if (ferror(fp) || feof(fp)) {
		fclose(fp);
		perror("write_disk_block_info(): read file wrong");
		return -1;
	}
	fclose(fp);
	return 0;
}

static int strcnt(const char* str, const char ch){
	int cnt = 0;
	while(*str){
		if(*str == ch) ++cnt;
		++str;
	}
	return cnt;
}

static int check_path(const char * const path, char * const dirname,
                    char * const fname, char * const fext){
    if(strcmp(path, "/") == 0){
        return 0; //指根目录
    }
    if(strlen(path) <= 1){
        return -1; //路径不正确
    }
    int res = strcnt(path, '/');
    if(res == 1){
        int cnt = strcnt(path, '.');
        if(cnt == 0){
            sscanf(path, "/%s", fname);
            strcpy(fext, "");
            strcpy(dirname, "");
            if(strlen(fname) > MAX_FILENAME){
                return -2; //文件名过长
            }
            return 1; //根目录下，不带后缀名的文件或目录
        }
        else if(cnt == 1){
            sscanf(path, "/%[^.].%s", fname, fext);
            strcpy(dirname, "");
            if(strlen(fname) > MAX_FILENAME 
            || strlen(fext) > MAX_EXTENSION){
                return -2; //文件名过长
            }
            return 1; //根目录下，带后缀的文件
        }
        return -1; //路径有误
    }
    else if(res == 2){
        sscanf(path, "/%[^/]", dirname);
        if(strcnt(dirname, '.') != 0){
            return -1; //目录名不能带.
        }
        if(strlen(dirname) > MAX_FILENAME){
            return -2; //目录名过长
        }
        int cnt = strcnt(path, '.');
        if(cnt == 0){
            sscanf(path, "/%[^/]/%s", dirname, fname);
            strcpy(fext, "");
            if(strlen(fname) > MAX_FILENAME){
                return -2; //文件名过长
            }
            return 2;
        }
        else if(cnt == 1){
            sscanf(path, "/%[^/]/%[^.].%s", dirname, fname, fext);
            if(strlen(fname) > MAX_FILENAME 
            || strlen(fext) > MAX_EXTENSION){
                return -2; //文件名过长
            }
            if(strlen(fname) == 0){
                //文件名不能为空
                return -1; 
            }
            return 2;
        }
        return -1; //路径不正确
    }
    return -1;//路径不正确
}

static long read_stat_in_rootdir(const char* const fname, const char * const fext, 
                                struct u_fs_file_directory* f_dir){
    //you have to ensure that is under rootdir
    struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
    if(read_disk_block(0, disk_blk) == -1){
		printf("read_stat_in_rootdir(): read_disk_block failed\n");
		return -1;
	}
    long root_blk = ((struct sb*)disk_blk)->first_blk;

    struct u_fs_file_directory *dir;
    long curr_blk = 0; //目前在sb块
    long next_blk = root_blk; //下一步想读的是根目录块
    int offset = 0;
    while(next_blk != -1){
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk; //读完了，当前块移动到next_blk
        next_blk = disk_blk->nNextBlock;
        offset = 0;
        dir = (struct u_fs_file_directory *)disk_blk->data;
        while(offset < disk_blk->size){
            if(strcmp(dir->fname, fname) == 0
            && strcmp(dir->fext, fext) == 0)
            {
                strcpy(f_dir->fname, fname);
                strcpy(f_dir->fext, fext);
                f_dir->fsize = dir->fsize;
                f_dir->nStartBlock = dir->nStartBlock;
                f_dir->flag = dir->flag;
                free(disk_blk);
                return curr_blk; //返回这个项目在目录中的位置
            }
            dir++;
            offset += sizeof(struct u_fs_file_directory);
        }
    }
    free(disk_blk);
    return -1;
}

static long read_stat_from_block(const char* const fname, const char* const fext, 
                                        const long blk, struct u_fs_file_directory* f_dir)
{
    //you have to ensure that block not wrong
    struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
    struct u_fs_file_directory *dir;
    long curr_blk = -1; //目前在sb块
    long next_blk = blk; //下一步想读的是blk块
    int offset = 0;
    while(next_blk != -1){
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk; //读完了，当前块移动到next_blk
        next_blk = disk_blk->nNextBlock;
        offset = 0;
        dir = (struct u_fs_file_directory *)disk_blk->data;
        while(offset < disk_blk->size){
            if(strcmp(dir->fname, fname) == 0
            && strcmp(dir->fext, fext) == 0)
            {
                strcpy(f_dir->fname, fname);
                strcpy(f_dir->fext, fext);
                f_dir->fsize = dir->fsize;
                f_dir->nStartBlock = dir->nStartBlock;
                f_dir->flag = dir->flag;
                free(disk_blk);
                return curr_blk; //返回这个项目在目录中的位置
            }
            dir++;
            offset += sizeof(struct u_fs_file_directory);
        }
    }
    free(disk_blk);
    return -1;
}

static int write_stat_from_block(const long blk, struct u_fs_file_directory const * const f_dir){
    struct u_fs_disk_block *disk_blk;
	disk_blk = malloc(sizeof(struct u_fs_disk_block));
    read_disk_block(blk, disk_blk);
    struct u_fs_file_directory *it;
    it = (struct u_fs_file_directory *)disk_blk->data;
    int offset = 0;
    while(offset < disk_blk->size){ 
        if(f_dir->flag == it->flag &&
            strcmp(f_dir->fname, it->fname) == 0 &&
            strcmp(f_dir->fext, it->fext) == 0)
        {   
            cp_item(it, f_dir);
            write_disk_block(blk, disk_blk);
            free(disk_blk);
            return 0;
        }
        offset += sizeof(struct u_fs_file_directory);
        it++;
    }
    printf("write_stat_from_block(): can't find the item!\n");
    free(disk_blk);
    return -1;
}

static long read_stat_from_path(const char* path, struct u_fs_file_directory* f_dir){
    char dirname[2*MAX_FILENAME + 1];
    char fname[2*MAX_FILENAME + 1];
    char fext[2*MAX_EXTENSION + 1];
    int res = check_path(path, dirname, fname, fext);
    if(res < 0){
        return res;//-2路径中有名字过长, -1路径有误
    }
    if(res == 0){
        struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
        if(read_disk_block(0, disk_blk) == -1){
		    printf("read_stat_from_path(): read_disk_block failed!\n");
		    return -1;
	    }
        strcpy(f_dir->fname, "root");
        strcpy(f_dir->fext, "");
        f_dir->fsize = ((struct sb*)disk_blk)->fs_size * BLOCK_SIZE;
        f_dir->nStartBlock = ((struct sb*)disk_blk)->first_blk;
        f_dir->flag = 2;
        free(disk_blk);
        return 0; //返回超级块所在位置
	}
	if(res == 1){
		//根目录下的文件或目录 
        return read_stat_in_rootdir(fname, fext, f_dir);
	}
	if(res == 2){
		//子目录下的文件
        struct u_fs_file_directory* temp_dir = malloc(sizeof(struct u_fs_file_directory));
        long dres =  read_stat_in_rootdir(dirname, "", temp_dir);
        if(dres == -1){
			printf("read_stat_from_path(): subdirectory doesn't existed!\n");
            free(temp_dir);
            return -1;
        }
        dres = read_stat_from_block(fname, fext, temp_dir->nStartBlock, f_dir);
        free(temp_dir);
        return dres;
	}
    return -1;
}

static long enlarge_a_block(const long num_block, struct u_fs_disk_block * const disk_blk){
    long new_block = -1;
    if(get_consecutive_free_blocks(1, &new_block) == -1){
        printf("enlarge_a_block(): get a free block failed!\n");
        return -1;
    }
    //格式化新块
    struct u_fs_disk_block *tmp = malloc(sizeof(struct u_fs_disk_block));
    read_disk_block(new_block, tmp);
    tmp->size = 0;
    tmp->nNextBlock = -1;
    tmp->data[0] = '\0';
    write_disk_block(new_block, tmp);
    free(tmp);
    tmp = NULL;
    //disk_blk更新，同时写回磁盘
    disk_blk->nNextBlock = new_block;
    write_disk_block(num_block, disk_blk);  
    return new_block;
}

static int clear_blocks(const long start_blk){
    if(start_blk == -1){
        return -1;
    }
    struct u_fs_disk_block* disk_blk;
    disk_blk = malloc(sizeof(struct u_fs_disk_block));

    long curr_blk = start_blk;
    long next_blk = -1;
    while(curr_blk != -1){
        read_disk_block(curr_blk, disk_blk);
        next_blk = disk_blk->nNextBlock;
        disk_blk->size = 0;
        disk_blk->nNextBlock = -1;
        disk_blk->data[0] = '\0';
        write_disk_block(curr_blk, disk_blk);
        set_single_bit_in_bitmap(curr_blk, 0);
        curr_blk = next_blk;
    }
    free(disk_blk);
    disk_blk = NULL;
    return 0;
}

static int set_single_bit_in_bitmap(const long num, const int flag) {
	if (num == -1){
		return -1;
    }
	FILE* fp;
	fp = fopen(DISKIMG_PATH, "r+");
	if (fp == NULL){
		return -1;
    }

	fseek(fp, BLOCK_SIZE + (num/8), SEEK_SET);
	BYTE *byte = malloc(sizeof(BYTE));
	fread(byte, sizeof(BYTE), 1, fp);
    BYTE mask = (1<<7);
    mask >>= (num%8);
	if (flag){
		*byte |= mask;
    }
	else{
        *byte &= ~mask;
    }

	fseek(fp, BLOCK_SIZE + (num/8), SEEK_SET);
	fwrite(byte, sizeof(BYTE), 1, fp);
	fclose(fp);
	free(byte);
	return 0;
}

static int get_consecutive_free_blocks(const long num, long* start_blk){
    FILE *fp;
    fp = fopen(DISKIMG_PATH, "r+");
    if(fp == NULL){
        printf("get_free_blocks(): file open failed");
        return -1;
    }
    //检索bitmap，查找连续的
    long sum_cnt = 0;
    long ibit = 1 + NUM_BITMAP_BLOCK + 1;
    BYTE *byte;
    byte = malloc(sizeof(BYTE));
    long cnt = 0;
    long res_start_blk = ibit;
    int flag = 0;
    while(ibit < NUM_TOTAL_BLOCK - 1){
        long l_off = ibit % 8;
        BYTE mask = (1<<7);
        mask >>= l_off;
        fseek(fp, BLOCK_SIZE + (ibit/8), SEEK_SET);
        fread(byte, sizeof(BYTE), 1, fp);
        int i = l_off;
        for(; i < 8; i++){
            if((*byte&mask)!=mask){ //该位为0,空闲
                ++cnt;
                ++sum_cnt;
                if(cnt == num){
                    *start_blk = res_start_blk;
                    flag = 1;
                    break;
                }
            }
            else{
                cnt = 0;
                res_start_blk = ibit + 1;
            }
            mask >>= 1;
            ++ibit;
        }
        if(flag == 1) break;
    }
    free(byte);
    fclose(fp);

    if(flag == 0){ //没找到足够大的连续的空闲块
        return sum_cnt;
    }
    //可优化，write bitmap;
    int j = res_start_blk;
    int i;
    for(i = 0; i < num; i++){
        if(set_single_bit_in_bitmap(j, 1) == -1){
            return -2; //error
        }
        ++j;
    }
    return -1; //success
}

static void cp_item(struct u_fs_file_directory * const dest, struct u_fs_file_directory const * const src){
    strcpy(dest->fname, src->fname);
    strcpy(dest->fext, src->fext);
    dest->fsize = src->fsize;
    dest->nStartBlock = src->nStartBlock;
    dest->flag = src->flag;
}

static void reset_item(struct u_fs_file_directory * const dest){
    strcpy(dest->fname, "");
    strcpy(dest->fext, "");
    dest->fsize = 0;
    dest->nStartBlock = -1;
    dest->flag = 0;
}

static void move_to_last_item(struct u_fs_file_directory **it, struct u_fs_disk_block const * const db){
    int ceiling = sizeof(struct u_fs_file_directory);
    (*it) = (struct u_fs_file_directory *)db->data;
    while(ceiling < db->size){
        ceiling += sizeof(struct u_fs_file_directory);
        (*it)++;
    }
}

static int rm_item(const long i_blk, struct u_fs_file_directory const * const f_dir){
    struct u_fs_disk_block* disk_blk;
    disk_blk = malloc(sizeof(struct u_fs_disk_block));
    read_disk_block(i_blk, disk_blk);
    //删除项目所在的目录块中对应的一项
    struct u_fs_file_directory *it;
    struct u_fs_file_directory *last;
    it = (struct u_fs_file_directory *)disk_blk->data;
    last = (struct u_fs_file_directory *)disk_blk->data;
    int offset = 0;
    int flag = 0;
    while(offset < disk_blk->size){ 
        if(f_dir->flag == it->flag &&
            strcmp(f_dir->fname, it->fname) == 0 &&
            strcmp(f_dir->fext, it->fext) == 0)
        {   
            flag = 1;
            break;
        }
        offset += sizeof(struct u_fs_file_directory);
        it++;
    }
    if(flag == 0){
        printf("rm_item(): target item is not found!\n");
        return -1;
    }
    //首先删除其内容所在后续块
    clear_blocks(it->nStartBlock);
    //移动last指向该块最末尾
    move_to_last_item(&last, disk_blk);
    //把last数据覆盖到item上，并且清空last数据
    cp_item(it, last);
    reset_item(last);
    disk_blk->size -= sizeof(struct u_fs_file_directory);
    write_disk_block(i_blk, disk_blk);
    //上面的步骤把i_blk最后的项目覆盖到想要删除的项上，并已经项目清空了后续块
    
    long curr_blk = i_blk;
    long next_blk = disk_blk->nNextBlock;
    struct u_fs_disk_block* next_disk_blk;
    next_disk_blk = malloc(sizeof(struct u_fs_disk_block));
    while(next_blk != -1){
        //读下一块的内容
        read_disk_block(curr_blk, disk_blk); 
        read_disk_block(next_blk, next_disk_blk);
        //两个块item指针都指向最后，前面的块需要多增一位
        move_to_last_item(&it, disk_blk);
        it++;
        move_to_last_item(&last, next_disk_blk);
        //it内容用last覆盖，写回前块
        cp_item(it, last);
        disk_blk->size += sizeof(struct u_fs_file_directory);
        reset_item(last);
        next_disk_blk->size -= sizeof(struct u_fs_file_directory);
        if(next_disk_blk->size == 0){
            clear_blocks(next_blk);
            next_blk = -1;
            disk_blk->nNextBlock = -1;
            write_disk_block(curr_blk, disk_blk);
        }
        else{
            write_disk_block(curr_blk, disk_blk);
            write_disk_block(next_blk, next_disk_blk);
            curr_blk = next_blk;
            next_blk = next_disk_blk->nNextBlock;
        }
    } 
    return 0;
}

static int u_fs_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
{
	(void) fi;

	struct u_fs_file_directory* attr;
	attr = malloc(sizeof(struct u_fs_file_directory));
	long res = read_stat_from_path(path, attr);
	if(res == -1){
		free(attr);
		return -ENOENT;
	}
	memset(stbuf, 0, sizeof(struct stat));
	if(attr->flag == 2){
		stbuf->st_mode = S_IFDIR | 0666;
        stbuf->st_size = attr->fsize;
	}
	else if(attr->flag == 1){
		stbuf->st_mode = S_IFREG | 0666;
		stbuf->st_size = attr->fsize;
	}
	free(attr);
    attr = NULL;
	return 0;
}

static int u_fs_mkdir(const char *path, mode_t mode){
    (void) mode;

	if(strcmp(path, "/") == 0 || strcnt(path, '/') > 1
    || strcnt(path, '.') != 0){
		return -EPERM;
	}
    if(strlen(path) > (MAX_FILENAME - 1)){ //目录名过长
        return ENAMETOOLONG;
    }
    char dirname[MAX_FILENAME + 1];
    sscanf(path, "/%s", dirname);

	//读sb找到根目录块
	struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
	if(read_disk_block(0, disk_blk) == -1){
		printf("u_fs_mkdir(): read_disk_block failed\n");
		return -errno;
	}
	long root_blk = ((struct sb*)disk_blk)->first_blk;

	//遍历根目录
    struct u_fs_file_directory *dir = (struct u_fs_file_directory *)disk_blk->data;
    long curr_blk = 0; //目前在sb块
    long next_blk = root_blk; //下一步想读的是根目录块
    int offset = 0;
    while(next_blk != -1){
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk; //读完了，当前块移动到next_blk
        next_blk = disk_blk->nNextBlock;
        offset = 0;
        dir = (struct u_fs_file_directory *)disk_blk->data;
        while(offset < disk_blk->size){
            if(strcmp(dir->fname, dirname) == 0
            && strcmp(dir->fext, "") == 0){
                free(disk_blk);
                return -EEXIST; //存在同名的文件或目录
            }
            dir++;
            offset += sizeof(struct u_fs_file_directory);
        }
    }

	/**
	 * 遍历结束(此时没有重名)，添加新目录项给根目录，同时分配新目录空间
	 */
	if((offset + sizeof(struct u_fs_file_directory)) > MAX_DATA_IN_BLOCK)
	{ //根目录的这个块刚好满了
        long free_blk = enlarge_a_block(curr_blk, disk_blk);
		if(free_blk == -1){
            printf("u_fs_mkdir(): enlarge wrong!\n");
            free(disk_blk);
            return -errno;
        }
        read_disk_block(free_blk, disk_blk);
        curr_blk = free_blk;
        next_blk = -1;
	}
	//添加新目录项，并写回
	long free_blk = -1;
	if(get_consecutive_free_blocks(1, &free_blk) != -1){
		printf("No more space to mk or something error!");
		return -EPERM;
	}
    //在根目录添加一条新纪录
	strcpy(dir->fname, dirname);
	strcpy(dir->fext, "");
	dir->fsize = BLOCK_SIZE;
	dir->nStartBlock = free_blk;
	dir->flag = 2; //for directory
	disk_blk->size += sizeof(struct u_fs_file_directory);
	write_disk_block(curr_blk, disk_blk);
    //分配新目录空间，写回
	read_disk_block(free_blk, disk_blk);
	disk_blk->size = 0;
	disk_blk->nNextBlock = -1;
    disk_blk->data[0] = '\0';
	write_disk_block(free_blk, disk_blk);
	free(disk_blk);
	return 0;
}

static int u_fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
    (void) offset;
	(void) fi;
	(void) flags;
    if(strcmp(path, "/") == 0){ //读根目录
        struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
        if(read_disk_block(0, disk_blk) == -1){
            printf("u_fs_readdir(): read_disk_block failed\n");
            return -errno;
        }
        long root_blk = ((struct sb*)disk_blk)->first_blk;
        filler(buf, ".", NULL, 0, 0); //printf(".\n");
        filler(buf, "..", NULL, 0, 0); //printf("..\n");
        //遍历根目录
        struct u_fs_file_directory *dir;
        long curr_blk = 0; //目前在sb块
        long next_blk = root_blk; //下一步想读的是根目录块
        int offs = 0;
        while(next_blk != -1){
            read_disk_block(next_blk, disk_blk);
            curr_blk = next_blk; //读完了，当前块移动到next_blk
            next_blk = disk_blk->nNextBlock;
            offs = 0;
            dir = (struct u_fs_file_directory *)disk_blk->data;
            while(offs < disk_blk->size){
                if(strcmp(dir->fext, "") ==0){
                    filler(buf, dir->fname, NULL, 0, 0);
                }
                else{
                    char fdname[MAX_FILENAME + MAX_EXTENSION + 2];
                    strcpy(fdname, dir->fname);
                    strcat(fdname, ".");
                    strcat(fdname, dir->fext);
                    filler(buf, fdname, NULL, 0, 0);
                }
                dir++;
                offs += sizeof(struct u_fs_file_directory);
            }
        }
        free(disk_blk);
        return 0;
    }
    if(strcnt(path, '/') > 1 || strlen(path) > (MAX_FILENAME + 1)
    || strcnt(path, '.') != 0)
    {
        return -ENOENT;
    }
    char dirname[MAX_FILENAME + 1];
    sscanf(path, "/%s", dirname);
    struct u_fs_file_directory *tmp;
    tmp = malloc(sizeof(struct u_fs_file_directory));
    long curr_blk = read_stat_in_rootdir(dirname, "", tmp);
    if(curr_blk == -1 || tmp->flag != 2){ //找不到或找到的不是目录
        return -ENOENT;
    }
    //开始遍历指定目录
    struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
    filler(buf, ".", NULL, 0, 0); //printf(".\n");
    filler(buf, "..", NULL, 0, 0); //printf("..\n");
    //遍历目录
    struct u_fs_file_directory *dir;
    //curr_blk目前在根目录块
    long next_blk = tmp->nStartBlock; //下一步想读的是指定目录块
    free(tmp);
    int offs = 0;
    while(next_blk != -1){
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk; //读完了，当前块移动到next_blk
        next_blk = disk_blk->nNextBlock;
        offs = 0;
        dir = (struct u_fs_file_directory *)disk_blk->data;
        while(offs < disk_blk->size){
            if(strcmp(dir->fext, "") ==0){
                filler(buf, dir->fname, NULL, 0, 0);
            }
            else{
                char fdname[MAX_FILENAME + MAX_EXTENSION + 2];
                strcpy(fdname, dir->fname);
                strcat(fdname, ".");
                strcat(fdname, dir->fext);
                filler(buf, fdname, NULL, 0, 0);
            }
            dir++;
            offs += sizeof(struct u_fs_file_directory);
        }
    }
    free(disk_blk);
    return 0;
}

static void *u_fs_init(struct fuse_conn_info *conn, struct fuse_config *cfg){
	(void) conn;

	FILE *fp = NULL;
	fp = fopen(DISKIMG_PATH, "r+");
	if (fp == NULL) {
		fprintf(stderr, "u_fs init unsuccessful!\n");
		return NULL;
	}
	fseek(fp, 0, SEEK_SET);
	struct sb *sblk = malloc(sizeof(struct sb));
	fread(sblk, sizeof(struct sb), 1, fp);

	//init NUM_TOTAL_BLOCK!!!
	NUM_TOTAL_BLOCK = sblk->fs_size;
	fclose(fp);
	free(sblk);
	sblk = NULL;
	printf("u_fs init success!\n");
	return NULL;
}

static int u_fs_open(const char *path, struct fuse_file_info *fi){
    (void) path;
    (void) fi;
    return 0;
}

static int u_fs_truncate(const char *path, off_t size, struct fuse_file_info *fi){
    (void) path;
    (void) size;
    (void) fi;
    return 0;
}

static int u_fs_flush(const char *path, struct fuse_file_info *fi){
    (void) path;
    (void) fi;
    return 0;
}

static int u_fs_rmdir(const char *path){
	if(strcmp(path, "/") == 0 || strcnt(path, '/') > 1
    || strcnt(path, '.') != 0){ //目录名中不能包含‘.’
		return -ENOENT;
	}
    if(strlen(path) > (MAX_FILENAME - 1)){ //目录名过长
        return -ENOENT;
    }
    char dirname[MAX_FILENAME + 1];
    sscanf(path, "/%s", dirname);
	struct u_fs_file_directory* tmp_dir;
	tmp_dir = malloc(sizeof(struct u_fs_file_directory));
    long res = read_stat_in_rootdir(dirname, "", tmp_dir);
    if(res == -1){ //没搜索到内容
        free(tmp_dir);
        return -ENOENT;
    }
    else if(tmp_dir->flag != 2){ //搜到了，但不是目录
        free(tmp_dir);
        return -ENOTDIR;
    }
    //判断是不是空目录
	struct u_fs_disk_block* disk_blk;	
    disk_blk = malloc(sizeof(struct u_fs_disk_block));
	read_disk_block(tmp_dir->nStartBlock, disk_blk);
	if(disk_blk->size > 0){
		printf("u_fs_rmdir(): it is not a empty dir!\n");
		free(tmp_dir);
		free(disk_blk);
		return -ENOTEMPTY;
	}
	//是空目录，开始删除目录(res)
	if(rm_item(res, tmp_dir) == -1){
		printf("u_fs_rmdir(): rm_item() failed!\n");
		return -ENOENT;
	}
	return 0;
}

static int u_fs_mknod(const char *path, mode_t mode, dev_t rdev){
    (void) mode;
    (void) rdev;
    
    char dirname[2*MAX_FILENAME + 1];
    char fname[2*MAX_FILENAME + 1];
    char fext[2*MAX_EXTENSION + 1];
    int res = check_path(path, dirname, fname, fext);

    if(res == -2){
        return -ENAMETOOLONG;
    }
    if(res < 2){ //不允许在根目录下建文件，mknod里要求的
        return -EPERM;
    }
    
    struct u_fs_file_directory *tmp;
    tmp = malloc(sizeof(struct u_fs_file_directory));
    long curr_blk = read_stat_in_rootdir(dirname, "", tmp);
    if(curr_blk == -1 || tmp->flag != 2){ //提供的文件夹没找到
        return -EPERM;
    }
    
    //遍历二级目录
    struct u_fs_disk_block *disk_blk = malloc(sizeof(struct u_fs_disk_block));
    struct u_fs_file_directory *dir = (struct u_fs_file_directory *)disk_blk->data;
    //long curr_blk = 0; //目前在sb块
    long next_blk = tmp->nStartBlock;; //下一步想读的二级目录块
    free(tmp);
    int offset = 0;
    while(next_blk != -1){
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk; //读完了，当前块移动到next_blk
        next_blk = disk_blk->nNextBlock;
        offset = 0;
        dir = (struct u_fs_file_directory *)disk_blk->data;
        while(offset < disk_blk->size){
            if(strcmp(dir->fname, fname) == 0
            && strcmp(dir->fext, fext) == 0){
                free(disk_blk);
                return -EEXIST; //存在同名的文件
            }
            dir++;
            offset += sizeof(struct u_fs_file_directory);
        }
    }

	/**
	 * 遍历结束(此时没有重名)，添加新目录项给根目录，同时分配新文件空间
	 */
	if((offset + sizeof(struct u_fs_file_directory)) > MAX_DATA_IN_BLOCK)
	{ //二级目录的这个块刚好满了
        long free_blk = enlarge_a_block(curr_blk, disk_blk);
		if(free_blk == -1){
            printf("u_fs_mknod(): enlarge wrong!\n");
            free(disk_blk);
            return -errno;
        }
        read_disk_block(free_blk, disk_blk);
        curr_blk = free_blk;
        next_blk = -1;
	}
	//添加新目录项，并写回
	long free_blk = -1;
	if(get_consecutive_free_blocks(1, &free_blk) != -1){
		printf("No more space to mk or something error!");
		return -EPERM;
	}
    //在根目录添加一条新纪录
	strcpy(dir->fname, fname);
	strcpy(dir->fext, fext);
	dir->fsize = 0;
	dir->nStartBlock = free_blk;
	dir->flag = 1; //for file
	disk_blk->size += sizeof(struct u_fs_file_directory);
	write_disk_block(curr_blk, disk_blk);
    //分配新文件空间，写回
	read_disk_block(free_blk, disk_blk);
	disk_blk->size = 0;
	disk_blk->nNextBlock = -1;
    disk_blk->data[0] = '\0';
	write_disk_block(free_blk, disk_blk);
	free(disk_blk);
    return 0;
}

static int u_fs_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
    (void) fi;

	struct u_fs_file_directory* f_dir;
    f_dir = malloc(sizeof(struct u_fs_file_directory));
    //读取文件所在位置
    long curr_blk = read_stat_from_path(path, f_dir);
    if(curr_blk == -1){ //找不到文件
		free(f_dir);
		return -ENOENT;
	}
    if(f_dir->flag == 2){ //找到的是tmd目录
        free(f_dir);
		return -EISDIR;
    }

    if(offset > f_dir->fsize){
        free(f_dir);
        return 0; //offset跑出文件大小了，肯定读不对
    }
    
    struct u_fs_disk_block *disk_blk;
	disk_blk = malloc(sizeof(struct u_fs_disk_block));
    read_disk_block(f_dir->nStartBlock, disk_blk);
    curr_blk = f_dir->nStartBlock; //curr_blk在文件的起始块
    long next_blk = disk_blk->nNextBlock; //next_blk指向文件下一个块，如果没有下一个则为-1
    free(f_dir);
    f_dir = NULL;

    //首先根据offset移动到开始块（由于每个块能实际保存MAX_DATA_IN_BLOCK实际为496）
    long ignore_nblock = offset / MAX_DATA_IN_BLOCK;
    int i;
    for(i = 0; i < ignore_nblock; i++){
        if(next_blk == -1){//说明offset在文件尾，再读都没用了
            free(disk_blk);
            disk_blk = NULL;
            return 0;
        }
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk;
        next_blk = disk_blk->nNextBlock;
    }

    //可以开始写啦！curr_blk为当前块的位置哦
    //如果要追加数据，则每次执行memcpy后，要将目标数组地址增加到你要追加数据的地址
    off_t curr_offset = offset % MAX_DATA_IN_BLOCK;
    size_t buf_remain_size = size; //剩余要读的内容
    if(buf_remain_size <= (MAX_DATA_IN_BLOCK - curr_offset)){ //如果刚好这个块能读完
        memcpy(buf, disk_blk->data + curr_offset, buf_remain_size);
        free(disk_blk);
        disk_blk = NULL;
        return size; //退出，刚好读完所有
    } 
    //要读的size很大，得慢慢读了
    //先读完这个块
    memcpy(buf, disk_blk->data + curr_offset, (MAX_DATA_IN_BLOCK - curr_offset));
    //开始读剩下的块，肯定都是从块头开始读的
    buf_remain_size -= (MAX_DATA_IN_BLOCK - curr_offset);
    size_t r_size = 0 + (MAX_DATA_IN_BLOCK - curr_offset);
    size_t need_read = MAX_DATA_IN_BLOCK;
    while(buf_remain_size > 0){
        if(next_blk == -1){ //没有下一个块可以读了
            return r_size;
        }
        curr_blk = next_blk;
        next_blk = disk_blk->nNextBlock;
        read_disk_block(curr_blk, disk_blk);
        if(buf_remain_size <= MAX_DATA_IN_BLOCK){ //这个块读的完
            need_read = buf_remain_size;
        }
        memcpy(buf + r_size, disk_blk->data, need_read);
        r_size += need_read;
        buf_remain_size -= need_read;
    }
    free(disk_blk);
    disk_blk = NULL;
    return r_size; //退出，读成功
}
static int u_fs_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

	struct u_fs_file_directory* f_dir;
    f_dir = malloc(sizeof(struct u_fs_file_directory));
    //读取文件所在位置
    long file_addr = read_stat_from_path(path, f_dir);
    long curr_blk = file_addr;
    if(curr_blk == -1){ //找不到文件
		free(f_dir);
		return -ENOENT;
	}
    if(f_dir->flag == 2){ //找到的是tmd目录
        free(f_dir);
		return -EISDIR;
    }
    //size_t real_fsize = (f_dir->fsize / BLOCK_SIZE) * MAX_DATA_IN_BLOCK;
    if(offset > f_dir->fsize){
        free(f_dir);
        return -EFBIG;
    }
    
    if((offset + size) > f_dir->fsize){ //如果比原来的文件长，修改原先文件的长度
        f_dir->fsize = offset + size;
        write_stat_from_block(file_addr, f_dir);
    }

    struct u_fs_disk_block *disk_blk;
	disk_blk = malloc(sizeof(struct u_fs_disk_block));
    read_disk_block(f_dir->nStartBlock, disk_blk);
    curr_blk = f_dir->nStartBlock; //curr_blk在文件的起始块
    long next_blk = disk_blk->nNextBlock; //next_blk指向文件下一个块，如果没有下一个则为-1
    free(f_dir);
    f_dir = NULL;
    //首先根据offset移动到开始块（由于每个块能实际保存MAX_DATA_IN_BLOCK实际为496）
    long ignore_nblock = offset / MAX_DATA_IN_BLOCK;
    int i;
    for(i = 0; i < ignore_nblock; i++){
        if(next_blk == -1){ //这种情况只会在文件尾，且刚好块被填满的情况
            enlarge_a_block(curr_blk, disk_blk);
        }
        read_disk_block(next_blk, disk_blk);
        curr_blk = next_blk;
        next_blk = disk_blk->nNextBlock;
    }

    //可以开始写啦！curr_blk为当前块的位置哦
    //如果要追加数据，则每次执行memcpy后，要将目标数组地址增加到你要追加数据的地址
    off_t curr_offset = offset % MAX_DATA_IN_BLOCK;
    size_t buf_remain_size = size; //剩余要写的size
    if(buf_remain_size <= (MAX_DATA_IN_BLOCK - curr_offset)){ //如果刚好这个块能写完
        memcpy(disk_blk->data + curr_offset, buf, buf_remain_size);
        write_disk_block(curr_blk, disk_blk);
        free(disk_blk);
        disk_blk = NULL;
        return 0; //退出，写成功
    } 
    //一个块写不完
    //先写完这个块
    memcpy(disk_blk->data + curr_offset, buf, (MAX_DATA_IN_BLOCK - curr_offset));
    write_disk_block(curr_blk, disk_blk);
    //开始写剩下的块，肯定都是从块头开始写的
    buf_remain_size -= (MAX_DATA_IN_BLOCK - curr_offset);
    size_t w_size = 0 + (MAX_DATA_IN_BLOCK - curr_offset);
    size_t need_write = MAX_DATA_IN_BLOCK;
    while(buf_remain_size > 0){
        if(next_blk == -1){
            enlarge_a_block(curr_blk, disk_blk);
        }
        curr_blk = next_blk;
        next_blk = disk_blk->nNextBlock;
        read_disk_block(curr_blk, disk_blk);
        if(buf_remain_size <= MAX_DATA_IN_BLOCK){ //这个块写的完
            need_write = buf_remain_size;
        }
        memcpy(disk_blk->data, buf + w_size, need_write);
        write_disk_block(curr_blk, disk_blk);
        w_size += need_write;
        buf_remain_size -= need_write;
    }
    free(disk_blk);
    disk_blk = NULL;
    return 0; //退出，写成功
}
static int u_fs_unlink(const char *path){

    char dirname[2*MAX_FILENAME + 1];
    char fname[2*MAX_FILENAME + 1];
    char fext[2*MAX_EXTENSION + 1];
    int res = check_path(path, dirname, fname, fext);

    if(res < 1){ //路径有误
        return -EPERM;
    }
    if(res == 1){ //根目录下的文件
        struct u_fs_file_directory *tmp;
        tmp = malloc(sizeof(struct u_fs_file_directory));
        long curr_blk = read_stat_in_rootdir(fname, fext, tmp);

        if(curr_blk == -1){ //提供的文件没找到
            return -ENOENT;
        }
        if(tmp->flag == 2){ //找到的是目录
            return -EISDIR;
        }
        rm_item(curr_blk, tmp);
        free(tmp);
        return 0;
    }
    else if(res == 2){ //子目录下的文件
        struct u_fs_file_directory *tmp;
        tmp = malloc(sizeof(struct u_fs_file_directory));
        long curr_blk = read_stat_in_rootdir(dirname, "", tmp);
        if(curr_blk == -1 || tmp->flag != 2){ //提供的子目录没找到
            free(tmp);
            return -ENOENT;
        }
        curr_blk = read_stat_from_block(fname, fext, tmp->nStartBlock, tmp);
        if(curr_blk == -1){ //提供的文件没找到
            free(tmp);
            return -ENOENT;
        }
        if(tmp->flag == 2){ //这种情况是不可能出现的，但还是先写上
            free(tmp);
            return -EISDIR;
        }
        if(tmp->flag == 1){ //找到了文件，删除
            rm_item(curr_blk, tmp);
            free(tmp);
            return 0;
        }
        return -EPERM;
    }
    return -EPERM;
}
