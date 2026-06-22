#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int spermfs_inode_alloc(spermfs_context_t *ctx, spermfs_inode_t *inode,
                          uint64_t mode)
{
    if (!ctx || !inode) return SPERMAFS_ERR_INVAL;

    memset(inode, 0, sizeof(spermfs_inode_t));

    inode->inode_number = ctx->superblock.next_inode++;
    inode->mode = mode;
    inode->uid = 0;
    inode->gid = 0;
    inode->size = 0;
    inode->atime = spermfs_time_ns();
    inode->mtime = inode->atime;
    inode->ctime = inode->atime;
    inode->btime = inode->atime;
    inode->link_count = 1;
    inode->flags = 0;
    inode->parent_inode = 0;
    inode->name[0] = '\0';
    inode->compression_algo = SPERMAFS_COMPRESS_NONE;
    inode->encryption_algo = SPERMAFS_CRYPT_NONE;
    inode->num_extents = 0;

    spermfs_integrity_sign_inode(ctx, inode);

    return SPERMAFS_OK;
}

int spermfs_inode_read(spermfs_context_t *ctx, uint64_t inode_num,
                         spermfs_inode_t *inode)
{
    if (!ctx || !inode) return SPERMAFS_ERR_INVAL;
    if (inode_num == 0) return SPERMAFS_ERR_NOENT;

    /* Inodes are stored in blocks. Each block holds multiple inodes. */
    uint32_t bs = ctx->superblock.block_size;
    uint32_t inodes_per_block = bs / sizeof(spermfs_inode_t);
    if (inodes_per_block == 0) inodes_per_block = 1;

    uint64_t journal_end = ctx->superblock.journal_start > 0
        ? ctx->superblock.journal_start + ctx->superblock.journal_size
        : 2; /* before journal init: blocks 0=sb, 1=btree, 2+=inodes */
    uint64_t block_num = journal_end + (inode_num / inodes_per_block);
    uint32_t offset_in_block = (inode_num % inodes_per_block) * sizeof(spermfs_inode_t);

    /* Read the block */
    uint8_t *block_buf = malloc(bs);
    if (!block_buf) return SPERMAFS_ERR_NOMEM;

    int ret = spermfs_read_block(ctx, block_num, block_buf, bs, 0);
    if (ret != SPERMAFS_OK) {
        free(block_buf);
        return ret;
    }

    memcpy(inode, block_buf + offset_in_block, sizeof(spermfs_inode_t));
    free(block_buf);

    /* Check inode integrity */
    ret = spermfs_integrity_check_inode(ctx, inode);
    if (ret != SPERMAFS_OK)
        return SPERMAFS_ERR_CHECKSUM;

    return SPERMAFS_OK;
}

int spermfs_inode_write(spermfs_context_t *ctx, spermfs_inode_t *inode)
{
    if (!ctx || !inode) return SPERMAFS_ERR_INVAL;

    /* Update checksum and timestamps */
    inode->ctime = spermfs_time_ns();
    spermfs_integrity_sign_inode(ctx, inode);

    uint32_t bs = ctx->superblock.block_size;
    uint32_t inodes_per_block = bs / sizeof(spermfs_inode_t);
    if (inodes_per_block == 0) inodes_per_block = 1;

    uint64_t journal_end = ctx->superblock.journal_start > 0
        ? ctx->superblock.journal_start + ctx->superblock.journal_size
        : 2;
    uint64_t block_num = journal_end + (inode->inode_number / inodes_per_block);
    uint32_t offset_in_block = (inode->inode_number % inodes_per_block) * sizeof(spermfs_inode_t);

    /* Ensure inode block is tracked by the allocator */
    if (block_num + 1 > ctx->superblock.used_blocks)
        ctx->superblock.used_blocks = block_num + 1;

    /* Journal the change */
    spermfs_journal_log(ctx, inode->inode_number, 0, inode, sizeof(spermfs_inode_t));

    /* Read-modify-write the inode block */
    uint8_t *block_buf = malloc(bs);
    if (!block_buf) return SPERMAFS_ERR_NOMEM;

    int ret = spermfs_read_block(ctx, block_num, block_buf, bs, 0);
    if (ret != SPERMAFS_OK) {
        free(block_buf);
        return ret;
    }

    memcpy(block_buf + offset_in_block, inode, sizeof(spermfs_inode_t));

    ret = spermfs_write_block(ctx, block_num, block_buf, bs, 0);
    free(block_buf);

    return ret;
}

int spermfs_inode_free(spermfs_context_t *ctx, uint64_t inode_num)
{
    if (!ctx || inode_num == 0) return SPERMAFS_ERR_INVAL;

    spermfs_inode_t inode;
    int ret = spermfs_inode_read(ctx, inode_num, &inode);
    if (ret != SPERMAFS_OK) return ret;

    /* Free all extents */
    for (uint64_t i = 0; i < inode.num_extents; i++)
        spermfs_free_blocks(ctx, inode.extents[i].start, inode.extents[i].length);

    /* Clear inode */
    memset(&inode, 0, sizeof(inode));
    inode.inode_number = inode_num;
    inode.link_count = 0;
    spermfs_integrity_sign_inode(ctx, &inode);

    return spermfs_inode_write(ctx, &inode);
}
