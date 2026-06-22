#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_SNAPSHOTS 256

static spermfs_snapshot_t *snapshots[MAX_SNAPSHOTS];
static int num_snapshots = 0;

int spermfs_snapshot_create(spermfs_context_t *ctx, const char *name,
                              uint64_t *snap_id)
{
    if (!ctx || !name || !snap_id) return SPERMAFS_ERR_INVAL;
    if (num_snapshots >= MAX_SNAPSHOTS) return SPERMAFS_ERR_FULL;

    /* Allocate snapshot entry */
    spermfs_snapshot_t *snap = calloc(1, sizeof(spermfs_snapshot_t));
    if (!snap) return SPERMAFS_ERR_NOMEM;

    static uint64_t next_id = 1;
    snap->snapshot_id = next_id++;
    snap->parent_id = 0;
    snap->timestamp = spermfs_time_ns();
    snap->root_tree_root = ctx->superblock.root_tree_root;
    snap->flags = SPERMAFS_SNAP_READ_ONLY;
    snap->name_len = strlen(name);
    if (snap->name_len > 63) snap->name_len = 63;
    memcpy(snap->name, name, snap->name_len);
    snap->name[snap->name_len] = '\0';

    /* Store snapshot root in superblock */
    uint64_t snap_block;
    int ret = spermfs_alloc_blocks(ctx, 1, &snap_block, 0);
    if (ret != SPERMAFS_OK) { free(snap); return ret; }

    ret = spermfs_write_block(ctx, snap_block, snap, sizeof(spermfs_snapshot_t), 0);
    if (ret != SPERMAFS_OK) { free(snap); return ret; }

    /* Update snapshot chain */
    ctx->superblock.snapshot_root = snap_block;
    snapshots[num_snapshots++] = snap;
    *snap_id = snap->snapshot_id;

    fprintf(stderr, "SPERMAFS: snapshot #%llu '%s' created (block %llu)\n",
            (unsigned long long)snap->snapshot_id, name,
            (unsigned long long)snap_block);
    return SPERMAFS_OK;
}

int spermfs_snapshot_restore(spermfs_context_t *ctx, uint64_t snap_id)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    /* Find snapshot and restore root tree pointer */
    for (int i = 0; i < num_snapshots; i++) {
        if (snapshots[i]->snapshot_id == snap_id) {
            ctx->superblock.root_tree_root = snapshots[i]->root_tree_root;
            fprintf(stderr, "SPERMAFS: restored to snapshot #%llu '%s'\n",
                    (unsigned long long)snap_id, snapshots[i]->name);
            return SPERMAFS_OK;
        }
    }
    return SPERMAFS_ERR_NOENT;
}

int spermfs_snapshot_delete(spermfs_context_t *ctx, uint64_t snap_id)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    for (int i = 0; i < num_snapshots; i++) {
        if (snapshots[i]->snapshot_id == snap_id) {
            free(snapshots[i]);
            snapshots[i] = snapshots[--num_snapshots];
            fprintf(stderr, "SPERMAFS: snapshot #%llu deleted\n",
                    (unsigned long long)snap_id);
            return SPERMAFS_OK;
        }
    }
    return SPERMAFS_ERR_NOENT;
}

int spermfs_snapshot_list(spermfs_context_t *ctx,
                            void (*cb)(uint64_t id, const char *name, uint64_t time))
{
    if (!ctx || !cb) return SPERMAFS_ERR_INVAL;

    for (int i = 0; i < num_snapshots; i++)
        cb(snapshots[i]->snapshot_id, snapshots[i]->name, snapshots[i]->timestamp);

    return SPERMAFS_OK;
}

int spermfs_snapshot_diff(spermfs_context_t *ctx, uint64_t snap1_id, uint64_t snap2_id)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    spermfs_snapshot_t *s1 = NULL, *s2 = NULL;
    for (int i = 0; i < num_snapshots; i++) {
        if (snapshots[i]->snapshot_id == snap1_id) s1 = snapshots[i];
        if (snapshots[i]->snapshot_id == snap2_id) s2 = snapshots[i];
    }
    if (!s1 || !s2) return SPERMAFS_ERR_NOENT;

    printf("Diff between snapshot #%llu and #%llu:\n",
           (unsigned long long)snap1_id, (unsigned long long)snap2_id);
    printf("  Before root tree: block %llu\n",
           (unsigned long long)s1->root_tree_root);
    printf("  After  root tree: block %llu\n",
           (unsigned long long)s2->root_tree_root);

    if (s1->root_tree_root != s2->root_tree_root)
        printf("  Changes detected: B+Tree roots differ\n");
    else
        printf("  No changes\n");

    return SPERMAFS_OK;
}
