#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DEDUP_HASH_TABLE_SIZE 65536

typedef struct dedup_node {
    uint8_t     hash[SPERMAFS_DEDUP_HASH_SIZE];
    uint64_t    block_number;
    uint32_t    ref_count;
    uint32_t    block_size;
    struct dedup_node *next;
} dedup_node_t;

typedef struct {
    dedup_node_t *buckets[DEDUP_HASH_TABLE_SIZE];
    int           num_entries;
} dedup_table_t;

static uint32_t hash_hash(const uint8_t hash[32])
{
    uint32_t h = 0;
    for (int i = 0; i < 8; i++)
        h ^= ((uint32_t)hash[i * 4] | ((uint32_t)hash[i * 4 + 1] << 8) |
              ((uint32_t)hash[i * 4 + 2] << 16) | ((uint32_t)hash[i * 4 + 3] << 24));
    return h % DEDUP_HASH_TABLE_SIZE;
}

int spermfs_dedup_init(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    dedup_table_t *table = calloc(1, sizeof(dedup_table_t));
    if (!table) return SPERMAFS_ERR_NOMEM;

    ctx->dedup_table = table;
    return SPERMAFS_OK;
}

int spermfs_dedup_find(spermfs_context_t *ctx, const uint8_t *data, size_t len,
                         uint8_t hash[32], uint64_t *block_num)
{
    if (!ctx || !data || !hash || !block_num) return SPERMAFS_ERR_INVAL;
    if (!ctx->dedup_table) return SPERMAFS_ERR_DEDUP;

    /* Compute hash of data */
    int ret = spermfs_sha256(data, len, hash);
    if (ret != 0) return ret;

    dedup_table_t *table = (dedup_table_t *)ctx->dedup_table;
    uint32_t bucket = hash_hash(hash);

    dedup_node_t *node = table->buckets[bucket];
    while (node) {
        if (memcmp(node->hash, hash, 32) == 0) {
            *block_num = node->block_number;
            return SPERMAFS_OK; /* Found */
        }
        node = node->next;
    }

    return SPERMAFS_ERR_NOENT; /* Not found */
}

int spermfs_dedup_insert(spermfs_context_t *ctx, const uint8_t hash[32],
                           uint64_t block_num, size_t len)
{
    if (!ctx || !hash) return SPERMAFS_ERR_INVAL;
    if (!ctx->dedup_table) return SPERMAFS_ERR_DEDUP;

    dedup_table_t *table = (dedup_table_t *)ctx->dedup_table;
    uint32_t bucket = hash_hash(hash);

    /* Check if already exists */
    dedup_node_t *node = table->buckets[bucket];
    while (node) {
        if (memcmp(node->hash, hash, 32) == 0) {
            node->ref_count++;
            return SPERMAFS_OK;
        }
        node = node->next;
    }

    /* Insert new */
    dedup_node_t *new_node = calloc(1, sizeof(dedup_node_t));
    if (!new_node) return SPERMAFS_ERR_NOMEM;

    memcpy(new_node->hash, hash, 32);
    new_node->block_number = block_num;
    new_node->ref_count = 1;
    new_node->block_size = (uint32_t)len;
    new_node->next = table->buckets[bucket];
    table->buckets[bucket] = new_node;
    table->num_entries++;

    return SPERMAFS_OK;
}

int spermfs_dedup_ref(spermfs_context_t *ctx, uint64_t block_num)
{
    if (!ctx || !ctx->dedup_table) return SPERMAFS_ERR_DEDUP;

    dedup_table_t *table = (dedup_table_t *)ctx->dedup_table;

    for (int i = 0; i < DEDUP_HASH_TABLE_SIZE; i++) {
        dedup_node_t *node = table->buckets[i];
        while (node) {
            if (node->block_number == block_num) {
                node->ref_count++;
                return SPERMAFS_OK;
            }
            node = node->next;
        }
    }

    return SPERMAFS_ERR_NOENT;
}

int spermfs_dedup_unref(spermfs_context_t *ctx, uint64_t block_num)
{
    if (!ctx || !ctx->dedup_table) return SPERMAFS_ERR_DEDUP;

    dedup_table_t *table = (dedup_table_t *)ctx->dedup_table;

    for (int i = 0; i < DEDUP_HASH_TABLE_SIZE; i++) {
        dedup_node_t *node = table->buckets[i];
        dedup_node_t *prev = NULL;
        while (node) {
            if (node->block_number == block_num) {
                node->ref_count--;
                if (node->ref_count == 0) {
                    if (prev)
                        prev->next = node->next;
                    else
                        table->buckets[i] = node->next;
                    free(node);
                    table->num_entries--;
                }
                return SPERMAFS_OK;
            }
            prev = node;
            node = node->next;
        }
    }

    return SPERMAFS_ERR_NOENT;
}
