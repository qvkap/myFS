#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct cow_block {
    uint64_t old_block;
    uint64_t new_block;
    uint64_t inode_num;
    uint64_t logical_offset;
};

#define MAX_COW_PENDING 1024

static struct cow_block cow_pending[MAX_COW_PENDING];
static int cow_pending_count = 0;

int spermfs_cow_clone_block(spermfs_context_t *ctx, uint64_t old_block,
                              uint64_t *new_block)
{
    if (!ctx || !new_block) return SPERMAFS_ERR_INVAL;

    /* Allocate a new block */
    int ret = spermfs_alloc_blocks(ctx, 1, new_block, 0);
    if (ret != SPERMAFS_OK) return ret;

    /* Copy old data to new block */
    uint32_t bs = ctx->superblock.block_size;
    void *buf = malloc(bs);
    if (!buf) return SPERMAFS_ERR_NOMEM;

    ret = spermfs_read_block(ctx, old_block, buf, bs, 0);
    if (ret == SPERMAFS_OK) {
        ret = spermfs_write_block(ctx, *new_block, buf, bs, 0);
    }

    free(buf);
    return ret;
}

static int cow_find_pending(uint64_t old_block, uint64_t inode_num)
{
    for (int i = 0; i < cow_pending_count; i++) {
        if (cow_pending[i].old_block == old_block &&
            cow_pending[i].inode_num == inode_num)
            return i;
    }
    return -1;
}

int spermfs_cow_write(spermfs_context_t *ctx, spermfs_inode_t *inode,
                        const void *data, uint64_t offset, uint64_t size)
{
    if (!ctx || !inode || !data) return SPERMAFS_ERR_INVAL;

    uint32_t bs = ctx->superblock.block_size;
    uint64_t end_offset = offset + size;
    uint64_t start_block_idx = offset / bs;
    uint64_t end_block_idx = (end_offset + bs - 1) / bs;
    int ret;

    for (uint64_t bi = start_block_idx; bi < end_block_idx; bi++) {
        uint64_t block_offset = bi * bs;
        uint64_t in_block_offset = (offset > block_offset) ? offset - block_offset : 0;
        uint64_t write_size = size;
        if (in_block_offset + write_size > bs)
            write_size = bs - in_block_offset;

        /* Find which extent this block belongs to */
        uint64_t logical_block = bi;
        uint64_t phys_block = 0;
        int found = 0;

        for (uint64_t e = 0; e < inode->num_extents; e++) {
            if (logical_block >= inode->extents[e].start &&
                logical_block < inode->extents[e].start + inode->extents[e].length) {
                uint64_t offset_in_extent = logical_block - inode->extents[e].start;
                phys_block = inode->extents[e].start + offset_in_extent;
                found = 1;
                break;
            }
        }

        /* Check if we already cloned this block in this transaction */
        int pending_idx = cow_find_pending(phys_block, inode->inode_number);
        uint64_t write_block;

        if (pending_idx >= 0) {
            write_block = cow_pending[pending_idx].new_block;
        } else {
            /* Clone the block (COW) */
            ret = spermfs_cow_clone_block(ctx, phys_block, &write_block);
            if (ret != SPERMAFS_OK) return ret;

            /* Track for future writes in same transaction */
            if (cow_pending_count < MAX_COW_PENDING) {
                cow_pending[cow_pending_count].old_block = phys_block;
                cow_pending[cow_pending_count].new_block = write_block;
                cow_pending[cow_pending_count].inode_num = inode->inode_number;
                cow_pending[cow_pending_count].logical_offset = block_offset;
                cow_pending_count++;
            }

            /* Update extent to point to new block */
            if (found) {
                for (uint64_t e = 0; e < inode->num_extents; e++) {
                    if (logical_block >= inode->extents[e].start &&
                        logical_block < inode->extents[e].start + inode->extents[e].length) {
                        uint64_t offset_in_extent = logical_block - inode->extents[e].start;
                        inode->extents[e].start = write_block - offset_in_extent;
                        break;
                    }
                }
            } else {
                /* New allocation */
                spermfs_inode_add_extent(ctx, inode, write_block, 1);
            }
        }

        /* Write data to the new block */
        void *block_buf = malloc(bs);
        if (!block_buf) return SPERMAFS_ERR_NOMEM;

        ret = spermfs_read_block(ctx, write_block, block_buf, bs, 0);
        if (ret == SPERMAFS_OK) {
            memcpy((uint8_t *)block_buf + in_block_offset,
                   (const uint8_t *)data + (block_offset - (offset - (offset % bs))),
                   write_size);
            ret = spermfs_write_block(ctx, write_block, block_buf, bs, 0);
        }

        free(block_buf);
        if (ret != SPERMAFS_OK) return ret;
    }

    /* Update inode size */
    if (end_offset > inode->size)
        inode->size = end_offset;

    inode->mtime = spermfs_time_ns();
    return SPERMAFS_OK;
}

void spermfs_cow_commit(spermfs_context_t *ctx)
{
    (void)ctx;
    cow_pending_count = 0;
}

int spermfs_cow_read(spermfs_context_t *ctx, spermfs_inode_t *inode,
                       void *buf, uint64_t offset, uint64_t size)
{
    if (!ctx || !inode || !buf) return SPERMAFS_ERR_INVAL;

    if (offset + size > inode->size) {
        if (offset >= inode->size) return 0;
        size = inode->size - offset;
    }

    /* Check for embedded data */
    if (inode->flags & SPERMAFS_INODE_EMBEDDED) {
        if (offset + size <= SPERMAFS_EMBEDDED_MAX) {
            memcpy(buf, inode->embedded_data + offset, size);
            return (int)size;
        }
    }

    uint32_t bs = ctx->superblock.block_size;
    uint64_t end_offset = offset + size;
    uint64_t start_block_idx = offset / bs;
    uint64_t end_block_idx = (end_offset + bs - 1) / bs;
    size_t total_read = 0;

    for (uint64_t bi = start_block_idx; bi < end_block_idx; bi++) {
        uint64_t block_offset = bi * bs;
        uint64_t in_block_offset = (offset > block_offset) ? offset - block_offset : 0;
        uint64_t read_size = size - total_read;
        if (in_block_offset + read_size > bs)
            read_size = bs - in_block_offset;

        /* Find physical block from extents */
        uint64_t logical_block = bi;
        uint64_t phys_block = 0;
        int found = 0;

        for (uint64_t e = 0; e < inode->num_extents; e++) {
            if (logical_block >= inode->extents[e].start &&
                logical_block < inode->extents[e].start + inode->extents[e].length) {
                uint64_t offset_in_extent = logical_block - inode->extents[e].start;
                phys_block = inode->extents[e].start + offset_in_extent;
                found = 1;
                break;
            }
        }

        if (!found) {
            memset((uint8_t *)buf + total_read, 0, read_size);
            total_read += read_size;
            continue;
        }

        void *block_buf = malloc(bs);
        if (!block_buf) return SPERMAFS_ERR_NOMEM;

        int ret = spermfs_read_block(ctx, phys_block, block_buf, bs, 0);
        if (ret == SPERMAFS_OK) {
            memcpy((uint8_t *)buf + total_read,
                   (uint8_t *)block_buf + in_block_offset, read_size);
            total_read += read_size;
        }

        free(block_buf);
        if (ret != SPERMAFS_OK) return ret;
    }

    return (int)total_read;
}
