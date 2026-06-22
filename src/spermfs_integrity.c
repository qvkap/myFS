#include "spermfs.h"
#include <string.h>
#include <stdlib.h>

static int use_sha256 = 0;

void spermfs_integrity_set_sha256(int enabled)
{
    use_sha256 = enabled;
}

static uint64_t compute_data_checksum(const void *data, size_t size)
{
    if (use_sha256) {
        uint8_t hash[32];
        if (spermfs_sha256(data, size, hash) == 0) {
            uint64_t csum = 0;
            for (int i = 0; i < 8; i++)
                csum = (csum << 8) | hash[i];
            return csum;
        }
    }
    return spermfs_crc64(data, size, 0);
}

int spermfs_integrity_check_block(spermfs_context_t *ctx, uint64_t block_num,
                                    void *data, size_t size)
{
    (void)ctx;
    if (!data || size < sizeof(spermfs_block_header_t))
        return SPERMAFS_ERR_INVAL;

    spermfs_block_header_t *hdr = (spermfs_block_header_t *)data;
    uint64_t stored_csum = hdr->checksum;
    hdr->checksum = 0;

    uint64_t computed = compute_data_checksum(data, size);
    hdr->checksum = stored_csum;

    if (stored_csum != 0 && stored_csum != computed)
        return SPERMAFS_ERR_CHECKSUM;

    return SPERMAFS_OK;
}

int spermfs_integrity_write_block(spermfs_context_t *ctx, uint64_t block_num,
                                    void *data, size_t size)
{
    (void)ctx;
    if (!data || size < sizeof(spermfs_block_header_t))
        return SPERMAFS_ERR_INVAL;

    spermfs_block_header_t *hdr = (spermfs_block_header_t *)data;
    hdr->block_number = block_num;
    hdr->block_size = (uint32_t)size;
    hdr->checksum = 0;
    hdr->checksum = compute_data_checksum(data, size);

    return SPERMAFS_OK;
}

int spermfs_integrity_check_inode(spermfs_context_t *ctx, spermfs_inode_t *inode)
{
    (void)ctx;
    if (!inode) return SPERMAFS_ERR_INVAL;

    uint64_t stored = inode->checksum;
    inode->checksum = 0;
    uint64_t computed = compute_data_checksum(inode, sizeof(spermfs_inode_t));
    inode->checksum = stored;

    if (stored != 0 && stored != computed)
        return SPERMAFS_ERR_CHECKSUM;

    return SPERMAFS_OK;
}

int spermfs_integrity_sign_inode(spermfs_context_t *ctx, spermfs_inode_t *inode)
{
    (void)ctx;
    if (!inode) return SPERMAFS_ERR_INVAL;
    inode->checksum = 0;
    inode->checksum = compute_data_checksum(inode, sizeof(spermfs_inode_t));
    return SPERMAFS_OK;
}
