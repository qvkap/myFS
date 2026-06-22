#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Simple extent-based allocator using bitmaps stored in blocks.
   In a production system, this would use a B+Tree of free extents. */

#define BITS_PER_BLOCK(b) ((b) * 8)

typedef struct {
    uint64_t total_bits;
    uint64_t num_blocks;
    uint64_t *blocks_start;
    uint8_t **bitmaps;
    int      num_regions;
} extent_allocator_t;

static extent_allocator_t *allocator = NULL;

int spermfs_alloc_blocks(spermfs_context_t *ctx, uint64_t count,
                          uint64_t *start_block, int preferred_tier)
{
    (void)preferred_tier;
    if (!ctx || !start_block || count == 0) return SPERMAFS_ERR_INVAL;

    /* Simple sequential allocator using superblock tracking */
    spermfs_superblock_t *sb = &ctx->superblock;

    if (sb->used_blocks + count > sb->total_blocks)
        return SPERMAFS_ERR_NOSPACE;

    *start_block = sb->used_blocks;
    sb->used_blocks += count;

    return SPERMAFS_OK;
}

void spermfs_free_blocks(spermfs_context_t *ctx, uint64_t start, uint64_t count)
{
    (void)ctx;
    (void)start;
    (void)count;
    /* For simplicity, we don't reclaim blocks in this implementation.
       A real FS would merge extents and update free space tracking. */
}

int spermfs_inode_add_extent(spermfs_context_t *ctx, spermfs_inode_t *inode,
                               uint64_t start, uint64_t length)
{
    (void)ctx;
    if (!inode) return SPERMAFS_ERR_INVAL;
    if (inode->num_extents >= SPERMAFS_MAX_EXTENTS)
        return SPERMAFS_ERR_FULL;

    inode->extents[inode->num_extents].start = start;
    inode->extents[inode->num_extents].length = length;
    inode->num_extents++;
    return SPERMAFS_OK;
}

int spermfs_inode_remove_extent(spermfs_context_t *ctx, spermfs_inode_t *inode,
                                  uint64_t index)
{
    (void)ctx;
    if (!inode || index >= inode->num_extents) return SPERMAFS_ERR_INVAL;

    for (uint64_t i = index; i < inode->num_extents - 1; i++)
        inode->extents[i] = inode->extents[i + 1];
    inode->num_extents--;
    return SPERMAFS_OK;
}

int spermfs_read_block(spermfs_context_t *ctx, uint64_t block_num,
                        void *buf, size_t size, int tier)
{
    if (!ctx || !buf) return SPERMAFS_ERR_INVAL;

    if (tier >= ctx->num_tiers) tier = 0;
    int fd = ctx->tiers[tier].fd;
    if (fd < 0) return SPERMAFS_ERR_IO;

    uint64_t offset = block_num * ctx->superblock.block_size;
    if ((uint32_t)size > ctx->superblock.block_size)
        size = ctx->superblock.block_size;

    if (pread(fd, buf, size, (off_t)offset) != (ssize_t)size)
        return SPERMAFS_ERR_IO;

    return SPERMAFS_OK;
}

int spermfs_write_block(spermfs_context_t *ctx, uint64_t block_num,
                         void *buf, size_t size, int tier)
{
    if (!ctx || !buf) return SPERMAFS_ERR_INVAL;

    if (tier >= ctx->num_tiers) tier = 0;
    int fd = ctx->tiers[tier].fd;
    if (fd < 0) return SPERMAFS_ERR_IO;

    uint64_t offset = block_num * ctx->superblock.block_size;
    if ((uint32_t)size > ctx->superblock.block_size)
        size = ctx->superblock.block_size;

    if (pwrite(fd, buf, size, (off_t)offset) != (ssize_t)size)
        return SPERMAFS_ERR_IO;

    return SPERMAFS_OK;
}
