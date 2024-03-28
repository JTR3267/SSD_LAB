/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
#define min(a, b) ((a) < (b) ? (a) : (b))
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block: 16;
    } fields;
};
PCA_RULE curr_pca;

typedef struct write_cache
{
    size_t lba;
    char* data;
    struct write_cache* next;
}WRITE_CACHE;
WRITE_CACHE* head;
WRITE_CACHE* tail;
unsigned int cache_size;

typedef union erase_func_param ERASE_FUNC_PARAM;
union erase_func_param
{
    size_t byte_size;
    off_t offset;
    size_t lba;
};

unsigned int* L2P;
unsigned int* Block_status;
unsigned int threshold; //139, 145 maximum?

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024  )
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }

}

static int ssd_expand(size_t new_size)
{
    //logical size must be less than logic limit
    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //read from nand
    if ( (fptr = fopen(nand_name, "r") ))
    {
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}

static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //write to nand
    if ( (fptr = fopen(nand_name, "r+")))
    {
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size ++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block)
{
    char nand_name[100];
	int found = 0;
    FILE* fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    //erase nand
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }


	if (found == 0)
	{
		printf("nand erase not found\n");
        physic_size -= NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
		// return -EINVAL;
	}

    printf("nand erase %d pass\n", block);
    return 1;
}

static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */
	
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        //ssd is full, no pca can be allocated
        printf("No new PCA\n");
        return FULL_PCA;
    }

    //seq B
    if ( curr_pca.fields.page == (NAND_SIZE_KB * 1024 / 512) - 1)
    {
        //Block_status[block] = 0, block empty or using
        //Block_status[block] = 1, block full
        Block_status[curr_pca.fields.block] = 1;
        for(int block = 0; block < PHYSICAL_NAND_NUM; block++){
            if(Block_status[block] == 0){
                curr_pca.fields.block = block;
                break;
            }
        }
    }
    curr_pca.fields.page = (curr_pca.fields.page + 1 ) % (NAND_SIZE_KB * 1024 / 512);

    // if ( curr_pca.fields.block == PHYSICAL_NAND_NUM - 1)
    // {
    //     curr_pca.fields.page += 1;
    // }
    // curr_pca.fields.block = (curr_pca.fields.block + 1 ) % PHYSICAL_NAND_NUM;

    // if ( curr_pca.fields.page >= (NAND_SIZE_KB * 1024 / 512) )
    // {
    //     printf("No new PCA\n");
    //     curr_pca.pca = FULL_PCA;
    //     return FULL_PCA;
    // }
    // else
    // {

    return curr_pca.pca;
    // }
}

static int ftl_gc()
{
    printf("do gc\n");
    int valid_page[PHYSICAL_NAND_NUM] = {0};
    int unmap_table[PHYSICAL_NAND_NUM][NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE];
    for(int lba = 0; lba < LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE; lba++){
        if(L2P[lba] != INVALID_PCA){
            PCA_RULE pca;
            pca.pca = L2P[lba];
            unmap_table[pca.fields.block][valid_page[pca.fields.block]++] = lba;
        }
    }

    int minimum_valid_page = NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    int target_block = 0;
    for(int block = 0; block < PHYSICAL_NAND_NUM; block++){
        if(Block_status[block] == 1 && valid_page[block] < minimum_valid_page){
            target_block = block;
            minimum_valid_page = valid_page[block];
        }
    }
    
    char* tmp_buf = calloc(512, sizeof(char));
    for(int i = 0; i < minimum_valid_page; i++){
        PCA_RULE pca;
        pca.pca = L2P[unmap_table[target_block][i]];
        nand_read(tmp_buf, pca.pca);
        pca.pca = get_next_pca();
        nand_write(tmp_buf, pca.pca);
        L2P[unmap_table[target_block][i]] = pca.pca;
    }
    free(tmp_buf);
    
    Block_status[target_block] = 0;
    return nand_erase(target_block);
}

static int cache_flush(){
    if(physic_size > threshold){
        ftl_gc();
    }

    int rst;
    PCA_RULE pca;
    pca.pca = get_next_pca();
    rst = nand_write(head->data, pca.pca);
    if (rst > 0)
    {
        L2P[head->lba] = pca.pca;
        WRITE_CACHE* tmp_cache = head;
        head = head->next;
        free(tmp_cache);
        cache_size--;
        return 512 ;
    }
    else
    {
        printf(" --> Write fail !!!");
        return -EINVAL;
    }
}

static int cache_read(char* buf, size_t lba){
    WRITE_CACHE* tmp_cache = head;
    while(tmp_cache){
        if(tmp_cache->lba == lba){
            memcpy(buf, tmp_cache->data, 512);
            return 1;
        }
        tmp_cache = tmp_cache->next;
    }
    return 0;
}

static void cache_erase(size_t lba){
    WRITE_CACHE* tmp_cache = head;
    WRITE_CACHE* prev_cache = NULL;
    while(tmp_cache){
        if(tmp_cache->lba == lba){
            if(prev_cache){
                prev_cache->next = tmp_cache->next;
            }
            else{
                head = tmp_cache->next;
            }
            free(tmp_cache);
            cache_size--;
            break;
        }
        prev_cache = tmp_cache;
        tmp_cache = tmp_cache->next;
    }
}

static int cache_write(const char* buf, size_t lba){
    cache_erase(lba);

    cache_size++;
    WRITE_CACHE* new_cache = malloc(sizeof(WRITE_CACHE));
    new_cache->lba = lba;
    new_cache->next = NULL;
    new_cache->data = calloc(512, sizeof(char));
    memcpy(new_cache->data, buf, 512);

    if(head){
        tail->next = new_cache;
    }
    else{
        head = new_cache;
    }
    tail = new_cache;

    if(cache_size > 10){
        return cache_flush();
    }

    return 512;
}

static int ftl_read( char* buf, size_t lba)
{
    PCA_RULE pca;

	pca.pca = L2P[lba];
	if (pca.pca == INVALID_PCA) {
	    //data has not be written, return 0
	    return cache_read(buf, lba);
	}
	else {
        return nand_read(buf, pca.pca);
	}
}

static int ftl_write(const char* buf, size_t byte_size, off_t offset, size_t lba)
{
    /*  TODO: only basic write case, need to consider other cases */
    int rst;
    if(byte_size < 512) {
        char* tmp_buf = calloc(512, sizeof(char));
        if(!ftl_read(tmp_buf, lba)){
            memset(tmp_buf, 0, 512);
        }
        L2P[lba] = INVALID_PCA;
        
        memcpy(tmp_buf + offset % 512, buf, byte_size);
        rst = cache_write(tmp_buf, lba);
        free(tmp_buf);
    }
    else{
        rst = cache_write(buf, lba);
    }

    if (rst > 0)
    {
        return 512 ;
    }
    else
    {
        printf(" --> Write fail !!!");
        return -EINVAL;
    }
}

static int ftl_erase(size_t byte_size, off_t offset, size_t lba)
{
    if(byte_size < 512){
        char* tmp_buf = calloc(512, sizeof(char));
        if(ftl_read(tmp_buf, lba)){
            L2P[lba] = INVALID_PCA;
            cache_erase(lba);

            char* zero_buf = calloc(512, sizeof(char));
            memset(zero_buf, 0, 512);
            memset(tmp_buf + offset % 512, 0, byte_size);
            if (memcmp(tmp_buf, zero_buf, 512) != 0) {
                ftl_write(tmp_buf, 512, offset, lba);
            }
            free(zero_buf);
        }
        free(tmp_buf);
    }
    else{
        L2P[lba] = INVALID_PCA;
        cache_erase(lba);
    }
    return 512;
}

static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}

static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}

static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, rst ;
    char* tmp_buf;

    // out of limit
    if ((offset ) >= logic_size)
    {
        return 0;
    }
    if ( size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
	tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        rst = ftl_read(tmp_buf + i * 512, tmp_lba++);
        if ( rst == 0)
        {
            //data has not be written, return empty data
            memset(tmp_buf + i * 512, 0, 512);
        }
        else if (rst < 0 )
        {
            free(tmp_buf);
            return rst;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}

static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static int ssd_do_erase(off_t eraseStart, size_t eraseSize, int write_bit)
{
    //TODO
    int tmp_lba, tmp_lba_range;
    int idx, curr_size, remain_size;

    if (eraseStart >= logic_size)
    {
        return 0;
    }
    if (eraseSize > logic_size - eraseStart)
    {
        //is valid data section
        eraseSize = logic_size - eraseStart;
    }

    tmp_lba = eraseStart / 512;
    tmp_lba_range = (eraseStart + eraseSize - 1) / 512 - (tmp_lba) + 1;

    remain_size = eraseSize;

    ERASE_FUNC_PARAM write_delay_array[2];
    int write_delay_size = 0;

    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        curr_size = 512;
        if(eraseStart % 512 != 0 && idx == 0){
            curr_size = min(512 * (eraseStart / 512 + 1), remain_size + eraseStart) - eraseStart;
        }
        else if(idx == tmp_lba_range - 1 && remain_size % 512 != 0){
            curr_size = remain_size;
        }

        if(curr_size < 512){
            write_delay_array[write_delay_size].byte_size = curr_size;
            write_delay_array[write_delay_size].offset = eraseStart;
            write_delay_array[write_delay_size].lba = tmp_lba + idx;
            write_delay_size++;
        }
        else{
            ftl_erase(curr_size, eraseStart, tmp_lba + idx);
        }

        remain_size -= curr_size;
        eraseStart += curr_size;
    }

    for(int i = 0;i < write_delay_size && write_bit; i++){
        ftl_erase(write_delay_array[i].byte_size, write_delay_array[i].offset, write_delay_array[i].lba);
    }

    return eraseSize;
}

static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    /*  TODO: only basic write case, need to consider other cases */
	
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    
    ssd_do_erase(offset, size, 0);

    process_size = 0;
    remain_size = size;
    // curr_size = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        /*  example only align 512, need to implement other cases  */
        curr_size = 512;
        if(offset % 512 != 0 && idx == 0){
            curr_size = min(512 * (offset / 512 + 1), remain_size + offset) - offset;
        }
        else if(idx == tmp_lba_range - 1 && remain_size % 512 != 0){
            curr_size = remain_size;
        }

        rst = ftl_write(buf + process_size, curr_size, offset, tmp_lba + idx);
        if ( rst == 0 ){
            //write full return -enomem;
            return -ENOMEM;
        }
        else if (rst < 0){
            //error
            return rst;
        }
        // curr_size += 512;
        remain_size -= curr_size;
        process_size += curr_size;
        offset += curr_size;
        // else{
        //     printf(" --> Not align 512 !!!");
        //     return -EINVAL;
        // }
    }

    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}

static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
        case SSD_LOGIC_ERASE:
            {
            unsigned long long eraseFrame = *(unsigned long long*) data;
            int eraseSize = eraseFrame & 0xFFFF;
            int eraseStart = (eraseFrame >> 32) & 0xFFFF;           
            printf(" --> erase start: %u, erase size: %u\n", eraseStart, eraseSize);
            ssd_do_erase(eraseStart, eraseSize, 1);
			}
            return 0;
    }
    return -EINVAL;
}

static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};

int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    threshold = 145;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

    Block_status = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(Block_status, 0, PHYSICAL_NAND_NUM * sizeof(int));

    head = NULL;
    tail = NULL;
    cache_size = 0;

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
