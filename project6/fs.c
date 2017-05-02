
#include "fs.h"
#include "disk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#define FS_MAGIC           0xf0f03410
#define INODES_PER_BLOCK   128
#define POINTERS_PER_INODE 5
#define POINTERS_PER_BLOCK 1024

struct fs_superblock {
    int magic;
    int nblocks;
    int ninodeblocks;
    int ninodes;
};

struct fs_inode {
    int isvalid;
    int size;
    int direct[POINTERS_PER_INODE];
    int indirect;
};

union fs_block {
    struct fs_superblock super;
    struct fs_inode inode[INODES_PER_BLOCK];
    int pointers[POINTERS_PER_BLOCK];
    char data[DISK_BLOCK_SIZE];
};

void inode_load( int inumber, struct fs_inode *inode) {
    int inode_block = inumber / INODES_PER_BLOCK + 1;
    int inode_num = inumber % INODES_PER_BLOCK;
    union fs_block x;
    disk_read(inode_block, x.data);
    memcpy(inode, &x.inode[inode_num], sizeof(struct fs_inode));
}

void inode_save( int inumber, struct fs_inode *inode) {
    int inode_block = inumber / INODES_PER_BLOCK + 1;
    int inode_num = inumber % INODES_PER_BLOCK;
    union fs_block x;
    disk_read(inode_block, x.data);
    memcpy(&x.inode[inode_num], inode, sizeof(struct fs_inode));
    x.inode[inode_num] = *inode;
    disk_write(inode_block, x.data);
}

int * FREE_BLOCK_MAP;
int MOUNTED = 0;

int fs_format()
{
    // check to see if disk is mounted
    if(MOUNTED) {
        printf("disk already mounted\n");
        return 0;
    }

    // if not mounted, clear the disk, reset inode table
    union fs_block block;
    disk_read(0,block.data);
    // loop through all previous inodes and invalidate them
    int i;
    for(i = 1; i <= block.super.ninodeblocks; i++) {
        union fs_block inode;
        disk_read(i,inode.data);
        // loop through each inode in this block
        int j;
        for(j = 0; j < INODES_PER_BLOCK; j++) {
            inode.inode[j].isvalid = 0;
        }
        disk_write(i,inode.data);
    }

    // otherwise setup new superblock
    union fs_block sblock;
    sblock.super.magic = FS_MAGIC;
    int nblocks = disk_size();
    sblock.super.nblocks = nblocks;
    int ninodes = nblocks/10;
    if(ninodes == 0) ninodes = 1;
    sblock.super.ninodeblocks = ninodes;
    sblock.super.ninodes = INODES_PER_BLOCK;
    disk_write(0,sblock.data);

    return 1;
}

void fs_debug()
{
    union fs_block block;

    disk_read(0,block.data);

    printf("superblock:\n");
    printf("    %d blocks\n",block.super.nblocks);
    printf("    %d inode blocks\n",block.super.ninodeblocks);
    printf("    %d inodes\n",block.super.ninodes);

    // loop through all inode blocks
    int i;
    for(i = 1; i <= block.super.ninodeblocks; i++) {
        union fs_block inode;
        disk_read(i,inode.data);
        // loop through each inode in this block
        int j;
        for(j = 0; j < INODES_PER_BLOCK; j++) {
            // only print info is the inode is valid
            if(inode.inode[j].isvalid == 1) {
                int inumber = INODES_PER_BLOCK*(i-1) + j;
                printf("inode %d\n", inumber);
                printf("    size: %d bytes\n", inode.inode[j].size);
                printf("    direct blocks:");
                // loop through direct pointers and print block if valid
                int k;
                for(k = 0; k < POINTERS_PER_INODE; k++) {
                    if(inode.inode[j].direct[k] != 0) {
                        printf(" %d", inode.inode[j].direct[k]);
                    }
                }
                printf("\n");
                // get indirect block
                if(inode.inode[j].indirect != 0) {
                    printf("    indirect block: %d\n", inode.inode[j].indirect);
                    union fs_block indirect;
                    disk_read(inode.inode[j].indirect, indirect.data);
                    // loop through indirect data blocks
                    printf("    indirect data blocks:");
                    int x;
                    for(x = 0; x < POINTERS_PER_BLOCK; x++) {
                        if(indirect.pointers[x] != 0) {
                            printf(" %d", indirect.pointers[x]);
                        }
                    }
                    printf("\n");
                }
            }
        }
    }
}

int fs_mount()
{
    union fs_block sblock;
    disk_read(0,sblock.data);
    /* size_t m = sblock.super.magic; */
    if(sblock.super.magic != FS_MAGIC) {
        printf("disk not formatted\n");
        return 0;
    }
    if(FREE_BLOCK_MAP) free(FREE_BLOCK_MAP);
    FREE_BLOCK_MAP = calloc(sblock.super.nblocks, sizeof(int));
    // super block is not valid
    FREE_BLOCK_MAP[0] = 1;
    // loop through all inodes and build free block bitmap
    int i;
    for(i = 1; i <= sblock.super.ninodeblocks; i++) {
        union fs_block inode;
        disk_read(i,inode.data);
        // inode block is not valid
        FREE_BLOCK_MAP[i] = 1;
        // loop through each inode in this block
        int j;
        for(j = 0; j < INODES_PER_BLOCK; j++) {
            if(inode.inode[j].isvalid == 1) {
                // loop through direct pointers and update map if valid
                int k;
                for(k = 0; k < POINTERS_PER_INODE; k++) {
                    if(inode.inode[j].direct[k] != 0) {
                        FREE_BLOCK_MAP[inode.inode[j].direct[k]] = 1;
                    }
                }
                // get indirect block
                if(inode.inode[j].indirect != 0) {
                    union fs_block indirect;
                    disk_read(inode.inode[j].indirect, indirect.data);
                    // loop through indirect data blocks and update map
                    int x;
                    for(x = 0; x < POINTERS_PER_BLOCK; x++) {
                        if(indirect.pointers[x] != 0) {
                            FREE_BLOCK_MAP[indirect.pointers[x]] = 1;
                        }
                    }
                }
            }
        }
    }
    MOUNTED = 1;
    return 1;
}

int fs_create()
{
    if(!MOUNTED) {
        printf("filesystem not mounted\n");
        return 0;
    }
    union fs_block super;
    disk_read(0,super.data);
    int i;
    // scan for available inode
    for(i = 1; i < super.super.ninodes; i++) {
        struct fs_inode x;
        inode_load(i, &x);
        if(x.isvalid == 0) {
            x.isvalid = 1;
            x.size = 0;
            inode_save(i, &x);
            return i;
        }
    }
    return 0;
}

int fs_delete( int inumber )
{
    if(!MOUNTED) {
        printf("filesystem not mounted\n");
        return 0;
    }
    struct fs_inode x;
    inode_load(inumber, &x);
    x.isvalid = 0;
    x.size = 0;
    int i;
    for(i = 0; i < POINTERS_PER_INODE; i++) {
        FREE_BLOCK_MAP[x.direct[i]] = 0;
    }
    union fs_block f;
    disk_read(x.indirect, f.data);
    for(i = 0; i < POINTERS_PER_BLOCK; i++) {
        if(f.pointers[i] > 0) {
            printf("releasing %d\n", f.pointers[i]);
            FREE_BLOCK_MAP[f.pointers[i]] = 0;
        }
    }
    inode_save(inumber, &x);
    return 1;
}

int fs_getsize( int inumber )
{
    if(!MOUNTED) {
        printf("filesystem not mounted\n");
        return -1;
    }
    struct fs_inode x;
    inode_load(inumber, &x);
    if(x.isvalid == 0) {
        printf("not a valid inode\n");
        return -1;
    }
    return x.size;
}

int fs_read( int inumber, char *data, int length, int offset ) {
    return 0;
}

int fs_write( int inumber, const char *data, int length, int offset ) {
    return 0;
}

