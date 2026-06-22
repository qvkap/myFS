#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Access frequency tracking for tier migration */
#define ACCESS_HISTORY_SIZE 1024

typedef struct {
    uint64_t inode_num;
    uint64_t access_count;
    uint64_t last_access;
    int      current_tier;
} access_record_t;

static access_record_t access_history[ACCESS_HISTORY_SIZE];
static int access_count = 0;

#define TIER_NVME_WEIGHT  100
#define TIER_SSD_WEIGHT   50
#define TIER_HDD_WEIGHT   10

int spermfs_tier_init(spermfs_context_t *ctx, const char *nvme_dev,
                        const char *ssd_dev, const char *hdd_dev)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    ctx->num_tiers = 0;

    if (nvme_dev) {
        spermfs_tier_device_t *tier = &ctx->tiers[SPERMAFS_TIER_NVME];
        strncpy(tier->device_path, nvme_dev, sizeof(tier->device_path) - 1);
        tier->tier_type = SPERMAFS_TIER_NVME;
        tier->fd = open(nvme_dev, O_RDWR);
        if (tier->fd >= 0) {
            tier->block_size = ctx->superblock.block_size;
            ctx->num_tiers++;
            fprintf(stderr, "SPERMAFS: tier NVMe: %s\n", nvme_dev);
        }
    }

    if (ssd_dev) {
        spermfs_tier_device_t *tier = &ctx->tiers[SPERMAFS_TIER_SSD];
        strncpy(tier->device_path, ssd_dev, sizeof(tier->device_path) - 1);
        tier->tier_type = SPERMAFS_TIER_SSD;
        tier->fd = open(ssd_dev, O_RDWR);
        if (tier->fd >= 0) {
            tier->block_size = ctx->superblock.block_size;
            ctx->num_tiers++;
            fprintf(stderr, "SPERMAFS: tier SSD: %s\n", ssd_dev);
        }
    }

    if (hdd_dev) {
        spermfs_tier_device_t *tier = &ctx->tiers[SPERMAFS_TIER_HDD];
        strncpy(tier->device_path, hdd_dev, sizeof(tier->device_path) - 1);
        tier->tier_type = SPERMAFS_TIER_HDD;
        tier->fd = open(hdd_dev, O_RDWR);
        if (tier->fd >= 0) {
            tier->block_size = ctx->superblock.block_size;
            ctx->num_tiers++;
            fprintf(stderr, "SPERMAFS: tier HDD: %s\n", hdd_dev);
        }
    }

    memset(access_history, 0, sizeof(access_history));
    access_count = 0;

    return ctx->num_tiers > 0 ? SPERMAFS_OK : SPERMAFS_ERR_IO;
}

int spermfs_tier_write(spermfs_context_t *ctx, int tier, uint64_t block_num,
                         const void *data, size_t size)
{
    if (!ctx || !data) return SPERMAFS_ERR_INVAL;
    if (tier >= ctx->num_tiers) tier = 0;

    return spermfs_write_block(ctx, block_num, (void *)data, size, tier);
}

int spermfs_tier_read(spermfs_context_t *ctx, int tier, uint64_t block_num,
                        void *buf, size_t size)
{
    if (!ctx || !buf) return SPERMAFS_ERR_INVAL;
    if (tier >= ctx->num_tiers) tier = 0;

    return spermfs_read_block(ctx, block_num, buf, size, tier);
}

int spermfs_tier_migrate(spermfs_context_t *ctx, uint64_t block_num,
                           int from_tier, int to_tier)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;
    if (from_tier >= ctx->num_tiers || to_tier >= ctx->num_tiers)
        return SPERMAFS_ERR_INVAL;

    uint32_t bs = ctx->superblock.block_size;
    void *buf = malloc(bs);
    if (!buf) return SPERMAFS_ERR_NOMEM;

    /* Read from source tier */
    int ret = spermfs_tier_read(ctx, from_tier, block_num, buf, bs);
    if (ret != SPERMAFS_OK) { free(buf); return ret; }

    /* Write to destination tier */
    ret = spermfs_tier_write(ctx, to_tier, block_num, buf, bs);

    free(buf);
    return ret;
}

int spermfs_tier_select(spermfs_context_t *ctx, uint64_t inode_num,
                          uint64_t access_count_val)
{
    (void)inode_num;
    if (!ctx || ctx->num_tiers == 0) return 0;

    /* Hot data -> NVMe, warm -> SSD, cold -> HDD */
    if (access_count_val > 100)
        return SPERMAFS_TIER_NVME;
    else if (access_count_val > 10)
        return SPERMAFS_TIER_SSD;
    else
        return SPERMAFS_TIER_HDD;
}

void spermfs_tier_track_access(uint64_t inode_num, int tier)
{
    /* Track access for tier migration decisions */
    for (int i = 0; i < access_count; i++) {
        if (access_history[i].inode_num == inode_num) {
            access_history[i].access_count++;
            access_history[i].last_access = spermfs_time_ns();
            access_history[i].current_tier = tier;
            return;
        }
    }

    /* New entry */
    if (access_count < ACCESS_HISTORY_SIZE) {
        access_history[access_count].inode_num = inode_num;
        access_history[access_count].access_count = 1;
        access_history[access_count].last_access = spermfs_time_ns();
        access_history[access_count].current_tier = tier;
        access_count++;
    }
}
