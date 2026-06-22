#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

static const uint64_t sb_offsets[SPERMAFS_NUM_SB_COPIES] = {
    SPERMAFS_SB_OFFSET_PRIMARY,
    SPERMAFS_SB_OFFSET_COPY1,
    SPERMAFS_SB_OFFSET_COPY2,
    SPERMAFS_SB_OFFSET_COPY3
};

int spermfs_super_init(spermfs_context_t *ctx, uint64_t total_blocks, uint32_t block_size)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    memset(&ctx->superblock, 0, sizeof(spermfs_superblock_t));
    spermfs_superblock_t *sb = &ctx->superblock;

    sb->magic = SPERMAFS_MAGIC;
    sb->version_major = SPERMAFS_VERSION_MAJOR;
    sb->version_minor = SPERMAFS_VERSION_MINOR;
    sb->version_patch = SPERMAFS_VERSION_PATCH;
    sb->block_size = block_size;
    sb->total_blocks = total_blocks;
    sb->used_blocks = 1;
    sb->features = SPERMAFS_FEATURE_ALL;
    sb->root_tree_root = 0;
    sb->journal_start = 0;
    sb->journal_size = 0;
    sb->journal_head = 0;
    sb->snapshot_root = 0;
    sb->next_inode = 2;
    sb->root_inode = 1;
    sb->archive_inode = 0;
    sb->max_name_len = SPERMAFS_MAX_NAME_LEN;
    sb->compression_algo = SPERMAFS_COMPRESS_ZSTD;
    sb->encryption_algo = SPERMAFS_CRYPT_NONE;
    sb->backup_sb_offsets[0] = SPERMAFS_SB_OFFSET_COPY1;
    sb->backup_sb_offsets[1] = SPERMAFS_SB_OFFSET_COPY2;
    sb->backup_sb_offsets[2] = SPERMAFS_SB_OFFSET_COPY3;

    spermfs_uuid_generate(sb->uuid);
    sb->checksum = spermfs_crc64(sb, sizeof(spermfs_superblock_t), 0);

    return SPERMAFS_OK;
}

int spermfs_super_save(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    spermfs_superblock_t *sb = &ctx->superblock;
    int ret = SPERMAFS_OK;

    sb->checksum = 0;
    sb->checksum = spermfs_crc64(sb, sizeof(spermfs_superblock_t), 0);

    for (int i = 0; i < SPERMAFS_NUM_SB_COPIES; i++) {
        int tier = 0;
        if (ctx->num_tiers > 0) {
            int fd = ctx->tiers[tier].fd;
            if (fd < 0) continue;
            if (pwrite(fd, sb, sizeof(spermfs_superblock_t), sb_offsets[i])
                != (ssize_t)sizeof(spermfs_superblock_t)) {
                fprintf(stderr, "SPERMAFS: failed to write superblock copy %d\n", i);
                ret = SPERMAFS_ERR_IO;
            }
        }
    }
    return ret;
}

int spermfs_super_load(spermfs_context_t *ctx, const char *device)
{
    if (!ctx || !device) return SPERMAFS_ERR_INVAL;

    strncpy(ctx->tiers[0].device_path, device, sizeof(ctx->tiers[0].device_path) - 1);
    ctx->tiers[0].fd = open(device, O_RDWR);
    if (ctx->tiers[0].fd < 0) {
        fprintf(stderr, "SPERMAFS: cannot open device %s: %s\n", device, strerror(errno));
        return SPERMAFS_ERR_IO;
    }
    ctx->num_tiers = 1;

    return spermfs_super_find_best(ctx, device);
}

int spermfs_super_find_best(spermfs_context_t *ctx, const char *device)
{
    (void)device;
    if (!ctx) return SPERMAFS_ERR_INVAL;

    int fd = ctx->tiers[0].fd;
    if (fd < 0) return SPERMAFS_ERR_IO;

    uint64_t best_validity = 0;
    int best_idx = -1;
    spermfs_superblock_t temp;

    for (int i = 0; i < SPERMAFS_NUM_SB_COPIES; i++) {
        if (pread(fd, &temp, sizeof(temp), sb_offsets[i]) != (ssize_t)sizeof(temp))
            continue;

        uint64_t saved_csum = temp.checksum;
        temp.checksum = 0;
        uint64_t calc_csum = spermfs_crc64(&temp, sizeof(temp), 0);

        if (saved_csum == calc_csum && temp.magic == SPERMAFS_MAGIC) {
            uint64_t validity = temp.version_major * 1000000ULL +
                                temp.version_minor * 1000ULL +
                                temp.total_blocks;
            if (validity > best_validity) {
                best_validity = validity;
                best_idx = i;
                memcpy(&ctx->superblock, &temp, sizeof(temp));
                ctx->superblock.checksum = saved_csum;
            }
        }
    }

    if (best_idx < 0) {
        fprintf(stderr, "SPERMAFS: no valid superblock found\n");
        return SPERMAFS_ERR_CORRUPT;
    }

    fprintf(stderr, "SPERMAFS: loaded superblock copy %d (offset %llu)\n",
            best_idx, (unsigned long long)sb_offsets[best_idx]);

    ctx->superblock.checksum = 0;
    ctx->superblock.checksum = spermfs_crc64(&ctx->superblock, sizeof(ctx->superblock), 0);

    return SPERMAFS_OK;
}

int spermfs_super_checksum(spermfs_superblock_t *sb)
{
    if (!sb) return SPERMAFS_ERR_INVAL;
    uint64_t saved = sb->checksum;
    sb->checksum = 0;
    uint64_t calc = spermfs_crc64(sb, sizeof(*sb), 0);
    sb->checksum = saved;
    return (saved == calc) ? SPERMAFS_OK : SPERMAFS_ERR_CHECKSUM;
}

void spermfs_super_dump(spermfs_superblock_t *sb)
{
    if (!sb) return;
    char uuid_str[SPERMAFS_UUID_STR_LEN];
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             sb->uuid[0], sb->uuid[1], sb->uuid[2], sb->uuid[3],
             sb->uuid[4], sb->uuid[5], sb->uuid[6], sb->uuid[7],
             sb->uuid[8], sb->uuid[9], sb->uuid[10], sb->uuid[11],
             sb->uuid[12], sb->uuid[13], sb->uuid[14], sb->uuid[15]);

    printf("SPERMAFS Superblock:\n");
    printf("  Magic:           0x%016llx\n", (unsigned long long)sb->magic);
    printf("  Version:         %u.%u.%u\n", sb->version_major, sb->version_minor, sb->version_patch);
    printf("  UUID:            %s\n", uuid_str);
    printf("  Block Size:      %u\n", sb->block_size);
    printf("  Total Blocks:    %llu\n", (unsigned long long)sb->total_blocks);
    printf("  Used Blocks:     %llu\n", (unsigned long long)sb->used_blocks);
    printf("  Features:        0x%016llx\n", (unsigned long long)sb->features);
    printf("  Root Tree:       block %llu\n", (unsigned long long)sb->root_tree_root);
    printf("  Journal:         block %llu (size %llu)\n",
           (unsigned long long)sb->journal_start, (unsigned long long)sb->journal_size);
    printf("  Snapshot Root:   block %llu\n", (unsigned long long)sb->snapshot_root);
    printf("  Next Inode:      %llu\n", (unsigned long long)sb->next_inode);
    printf("  Root Inode:      %llu\n", (unsigned long long)sb->root_inode);
    printf("  Compression:     %s\n",
           sb->compression_algo == SPERMAFS_COMPRESS_NONE ? "none" :
           sb->compression_algo == SPERMAFS_COMPRESS_LZ4 ? "LZ4" :
           sb->compression_algo == SPERMAFS_COMPRESS_ZSTD ? "ZSTD" :
           sb->compression_algo == SPERMAFS_COMPRESS_DEFLATE ? "DEFLATE" : "unknown");
    printf("  Encryption:      %s\n",
           sb->encryption_algo == SPERMAFS_CRYPT_NONE ? "none" :
           sb->encryption_algo == SPERMAFS_CRYPT_AES256_GCM ? "AES-256-GCM" :
           sb->encryption_algo == SPERMAFS_CRYPT_XCHACHA20_POLY ? "XChaCha20-Poly1305" : "unknown");
}
