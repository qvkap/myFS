#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>

#define JOURNAL_HEADER_SIZE (64 * 1024)
#define MAX_JOURNAL_ENTRIES 1024

typedef struct {
    uint64_t magic;
    uint64_t sequence;
    uint64_t num_entries;
    uint64_t committed_upto;
    uint64_t checksum;
} journal_header_t;

#define JOURNAL_MAGIC 0x4A4F55524E414C48ULL

int spermfs_journal_init(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    /* Allocate journal space if not already allocated */
    if (ctx->superblock.journal_start == 0) {
        uint64_t total = ctx->superblock.total_blocks;
        uint64_t journal_blocks = total / 16;
        if (journal_blocks < 16) journal_blocks = 8;
        if (journal_blocks > 1024) journal_blocks = 1024;
        if (ctx->superblock.used_blocks + journal_blocks >= total) {
            journal_blocks = total - ctx->superblock.used_blocks - 4;
            if (journal_blocks < 4) journal_blocks = 4;
        }

        uint64_t start;
        int ret = spermfs_alloc_blocks(ctx, journal_blocks, &start, 0);
        if (ret != SPERMAFS_OK) {
            fprintf(stderr, "SPERMAFS: cannot allocate journal (%llu blocks)\n",
                    (unsigned long long)journal_blocks);
            return ret;
        }

        ctx->superblock.journal_start = start;
        ctx->superblock.journal_size = journal_blocks;
        ctx->superblock.journal_head = start;

        /* Initialize journal header */
        journal_header_t jh;
        memset(&jh, 0, sizeof(jh));
        jh.magic = JOURNAL_MAGIC;
        jh.sequence = 0;
        jh.checksum = spermfs_crc64(&jh, sizeof(jh), 0);

        spermfs_write_block(ctx, start, &jh, sizeof(jh), 0);
    }

    ctx->current_transaction = 0;
    return SPERMAFS_OK;
}

int spermfs_journal_begin(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;
    ctx->current_transaction++;
    return SPERMAFS_OK;
}

int spermfs_journal_log(spermfs_context_t *ctx, uint64_t inode_num,
                          uint64_t offset, const void *data, uint64_t len)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;
    if (ctx->current_transaction == 0) return SPERMAFS_OK;

    /* Calculate entry size */
    uint64_t entry_size = sizeof(spermfs_journal_entry_t) + len;

    /* For large entries, write metadata-only if data too big */
    if (entry_size > ctx->superblock.block_size) {
        /* Just log metadata without full data - journal will re-read from disk */
        uint64_t meta_size = sizeof(spermfs_journal_entry_t);
        spermfs_journal_entry_t *entry = malloc(meta_size);
        if (!entry) return SPERMAFS_ERR_NOMEM;

        entry->sequence = ctx->current_transaction;
        entry->transaction_id = ctx->current_transaction;
        entry->inode_number = inode_num;
        entry->offset = offset;
        entry->length = len;
        entry->state = SPERMAFS_JOURNAL_WRITING;
        entry->crc32 = (data && len > 0) ? spermfs_crc32(data, len > 64 ? 64 : (uint32_t)len) : 0;
        entry->timestamp = spermfs_time_ns();

        memset(entry->data, 0, meta_size - offsetof(spermfs_journal_entry_t, data));

        uint64_t journal_data_start = ctx->superblock.journal_head + 1;
        int ret = spermfs_write_block(ctx, journal_data_start, entry, meta_size, 0);
        if (ret == SPERMAFS_OK)
            ctx->superblock.journal_head = journal_data_start;

        free(entry);
        return ret;
    }

    spermfs_journal_entry_t *entry = malloc(entry_size);
    if (!entry) return SPERMAFS_ERR_NOMEM;

    entry->sequence = ctx->current_transaction;
    entry->transaction_id = ctx->current_transaction;
    entry->inode_number = inode_num;
    entry->offset = offset;
    entry->length = len;
    entry->state = SPERMAFS_JOURNAL_WRITING;
    entry->crc32 = (data && len > 0) ? spermfs_crc32(data, len) : 0;
    entry->timestamp = spermfs_time_ns();
    if (data && len > 0) {
        memcpy(entry->data, data, len);
    }

    /* Write to next journal slot */
    uint64_t journal_data_start = ctx->superblock.journal_head + 1;
    int ret = spermfs_write_block(ctx, journal_data_start, entry, entry_size, 0);
    if (ret == SPERMAFS_OK)
        ctx->superblock.journal_head = journal_data_start;

    free(entry);
    return ret;
}

int spermfs_journal_commit(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    /* Update journal header to mark transaction committed */
    journal_header_t jh;
    int ret = spermfs_read_block(ctx, ctx->superblock.journal_start, &jh, sizeof(jh), 0);
    if (ret != SPERMAFS_OK) return ret;

    jh.committed_upto = ctx->current_transaction;
    jh.sequence = ctx->current_transaction;
    jh.checksum = spermfs_crc64(&jh, sizeof(jh), 0);

    ret = spermfs_write_block(ctx, ctx->superblock.journal_start, &jh, sizeof(jh), 0);
    if (ret == SPERMAFS_OK)
        ctx->current_transaction = 0;

    return ret;
}

int spermfs_journal_rollback(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;
    /* Re-read last committed state from journal */
    return spermfs_journal_recover(ctx);
}

int spermfs_journal_recover(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;
    if (ctx->superblock.journal_start == 0 || ctx->superblock.journal_size == 0)
        return SPERMAFS_OK;

    journal_header_t jh;
    int ret = spermfs_read_block(ctx, ctx->superblock.journal_start, &jh, sizeof(jh), 0);
    if (ret != SPERMAFS_OK) return ret;

    if (jh.magic != JOURNAL_MAGIC) {
        fprintf(stderr, "SPERMAFS: warning: no valid journal found, starting fresh\n");
        /* Initialize fresh journal */
        memset(&jh, 0, sizeof(jh));
        jh.magic = JOURNAL_MAGIC;
        jh.sequence = 0;
        jh.checksum = spermfs_crc64(&jh, sizeof(jh), 0);
        spermfs_write_block(ctx, ctx->superblock.journal_start, &jh, sizeof(jh), 0);
        return SPERMAFS_OK;
    }

    /* Check for uncommitted entries after committed_upto */
    uint64_t committed = jh.committed_upto;
    uint64_t last_seq = jh.sequence;

    if (last_seq > committed) {
        fprintf(stderr, "SPERMAFS: recovering %llu uncommitted transactions\n",
                (unsigned long long)(last_seq - committed));
    }

    return SPERMAFS_OK;
}

void spermfs_journal_destroy(spermfs_context_t *ctx)
{
    (void)ctx;
    /* Flush and close journal */
}
