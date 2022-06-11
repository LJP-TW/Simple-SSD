/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

#define PAGESIZE 512

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef struct pca_rule PCA_RULE;
struct pca_rule
{
    union
    {
        unsigned int pca;
        struct
        {
            unsigned int lba : 16;
            unsigned int nand: 16;
        };
    };
};

typedef struct state_rule STATE_RULE;
struct state_rule
{
    union
    {
        unsigned int state;
        struct 
        {
            unsigned int valid_count: 8;
            unsigned int stale_count: 8;
            unsigned int stale      : 16;
        };
    };
};

PCA_RULE curr_pca;
static unsigned int get_next_pca();

PCA_RULE* L2P;
STATE_RULE* block_state;
unsigned int* P2L, free_block_number;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size > NAND_SIZE_KB * 1024)
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
    //logic must less logic limit

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
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.nand);

    //read
    if ((fptr = fopen(nand_name, "r")))
    {
        fseek(fptr, my_pca.lba * PAGESIZE, SEEK_SET);
        fread(buf, 1, PAGESIZE, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return PAGESIZE;
}

static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.nand);

    //write
    if ((fptr = fopen(nand_name, "r+")))
    {
        fseek(fptr, my_pca.lba * PAGESIZE, SEEK_SET);
        fwrite(buf, 1, PAGESIZE, fptr);
        fclose(fptr);
        physic_size ++;
        block_state[my_pca.nand].valid_count++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += PAGESIZE;
    return PAGESIZE;
}

static int nand_erase(int block_index)
{
    char nand_name[100];
    FILE* fptr;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block_index);
    fptr = fopen(nand_name, "w");
    if (fptr == NULL)
    {
        printf("erase nand_%d fail", block_index);
        return 0;
    }
    fclose(fptr);
    block_state[block_index].state = FREE_BLOCK;
    return 1;
}

static unsigned int get_next_block()
{
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (block_state[(curr_pca.nand + i) % PHYSICAL_NAND_NUM].state == FREE_BLOCK)
        {
            curr_pca.nand = (curr_pca.nand + i) % PHYSICAL_NAND_NUM;
            curr_pca.lba = 0;
            free_block_number--;
            block_state[curr_pca.nand].state = 0;
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}

static unsigned int get_next_pca()
{
    if (curr_pca.pca == INVALID_PCA)
    {
        curr_pca.pca = 0;
        block_state[0].state = 0;
        free_block_number--;
        return curr_pca.pca;
    }

    if (curr_pca.lba == 9)
    {
        int temp = get_next_block();
        if (temp == OUT_OF_BLOCK)
        {
            return OUT_OF_BLOCK;
        }
        else if (temp == -EINVAL)
        {
            return -EINVAL;
        }
        else
        {
            return temp;
        }
    }
    else
    {
        curr_pca.lba += 1;
    }
    return curr_pca.pca;
}

static unsigned int ftl_get_pca(int lba)
{
    return L2P[lba].pca;
}

static int ftl_set_stale(unsigned int p)
{
    PCA_RULE pca;

    if (p == INVALID_PCA)
    {
        return 0;
    }

    pca.pca = p;

    block_state[pca.nand].stale_count += 1;
    block_state[pca.nand].stale |= (1 << pca.lba);

    return 1;
}

/*
 * 1. Check L2P to get PCA
 * 2. Send read data into tmp_buffer
 */
static int ftl_read(char* buf, int lba)
{
    PCA_RULE pca;
    int ret;
    
    pca.pca = L2P[lba].pca;

    if (pca.pca == INVALID_PCA)
    {
        return 0;
    }

    ret = nand_read(buf, pca.pca);

    return ret;
}

/*
 * 1. Allocate a new PCA address
 * 2. Send NAND-write cmd
 * 3. Update L2P table
 */
static int ftl_write(const char* buf, int lba_range, int lba)
{
    int pca;
    int ret;

    pca = get_next_pca();

    if (pca < 0 || pca == OUT_OF_BLOCK)
    {
        return 0;
    }

    ret = nand_write(buf, pca);

    L2P[lba].pca = pca;

    return ret;
}

/*
 * 1. Decide the target block to be erase
 * 2. Move all the valid data that in target block to another block
 * 3. Erase the target block when all the data in target block are stale
 * 4. Mark the target block as available
 * 5. Continue until the number of blocks that you erased reach your goal
 */
static int gc(void)
{
    // TODO
    return -1;
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
    int tmp_lba, ret;
    int idx, curr_size, remain_size;
    char* tmp_buf;

    //off limit
    if (offset >= logic_size)
    {
        return 0;
    }
    if (size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / PAGESIZE;
    tmp_buf = calloc(PAGESIZE, sizeof(char));

    idx = 0;
    curr_size = 0;

    for (remain_size = size; remain_size >= 0; remain_size -= PAGESIZE)
    {
        ret = ftl_read(tmp_buf, tmp_lba + idx);
        
        if (ret <= 0)
        {
            return ret;
        }

        memcpy(&buf[curr_size], tmp_buf, remain_size > PAGESIZE ? PAGESIZE : remain_size);

        idx += 1;
        curr_size += PAGESIZE;
    }

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

static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range;
    int idx, curr_size, remain_size;
    char* tmp_buf;
    int ret;
    unsigned int pca;

    if (!size)
    {
        return 0;
    }

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / PAGESIZE;
    tmp_lba_range = (offset + size - 1) / PAGESIZE - (tmp_lba) + 1;

    idx = 0;
    curr_size = 0;

    for (remain_size = size; remain_size >= PAGESIZE; remain_size -= PAGESIZE)
    {
        pca = ftl_get_pca(tmp_lba + idx);
        ret = ftl_write(&buf[PAGESIZE * idx], tmp_lba_range - idx, tmp_lba + idx);

        if (ret <= 0)
        {
            return ret;
        }

        ftl_set_stale(pca);

        idx += 1;
        curr_size += PAGESIZE;
    }

    if (remain_size)
    {
        tmp_buf = calloc(PAGESIZE, sizeof(char));

        //read
        ret = ftl_read(tmp_buf, tmp_lba + idx);

        if (ret <= 0)
        {
            free(tmp_buf);
            return ret;
        }
        
        //modify
        memcpy(tmp_buf, &buf[curr_size], remain_size);

        //write
        pca = ftl_get_pca(tmp_lba + idx);
        ret = ftl_write(tmp_buf, 1, tmp_lba + idx);

        if (ret <= 0)
        {
            free(tmp_buf);
            return ret;
        }

        ftl_set_stale(pca);

        free(tmp_buf);
    }

    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{

    (void) fi;
    
    printf("\n\n[WRITE] OFFSET %0#8lx | SIZE: %0#8lx\n\n", offset, size);

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
    memset(L2P, INVALID_PCA, sizeof(PCA_RULE) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(block_state, FREE_BLOCK, sizeof(STATE_RULE) * PHYSICAL_NAND_NUM);
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
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

static void debug(void)
{
    printf("[DEBUG]\n");
    
    printf("%lx / %lx\n", nand_write_size, host_write_size);

    for (int i = 0; i < PHYSICAL_NAND_NUM; ++i)
    {
        if (block_state[i].state == FREE_BLOCK)
        {
            printf("NAND_%d | Invalid\n", i);
        }
        else
        {
            printf("NAND_%d | V: %d | S: %d, %0#8x\n", i, 
                   block_state[i].valid_count,
                   block_state[i].stale_count,
                   block_state[i].stale);
        }
    }

    printf("[DEBUG END]\n");
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
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            return 0;
        case SSD_GET_WA:
            debug();
            *(double*)data = (double)nand_write_size / (double)host_write_size;
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
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;

    L2P = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(PCA_RULE));
    memset(L2P, INVALID_PCA, sizeof(PCA_RULE) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    block_state = malloc(PHYSICAL_NAND_NUM * sizeof(STATE_RULE));
    memset(block_state, FREE_BLOCK, sizeof(STATE_RULE) * PHYSICAL_NAND_NUM);

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