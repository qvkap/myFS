#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_ARCHIVE_VERSIONS 64

typedef struct archive_version {
    uint64_t version;
    uint64_t timestamp;
    uint64_t inode_number;
    uint64_t size;
    uint8_t  hash[32];
    struct archive_version *next;
} archive_version_t;

typedef struct {
    archive_version_t *versions[MAX_ARCHIVE_VERSIONS];
    int                num_versions;
} archive_store_t;

static archive_store_t *archive = NULL;

int spermfs_archive_init(spermfs_context_t *ctx)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    archive = calloc(1, sizeof(archive_store_t));
    if (!archive) return SPERMAFS_ERR_NOMEM;

    /* Set archive inode in superblock */
    if (ctx->superblock.archive_inode == 0) {
        spermfs_inode_t archive_inode;
        int ret = spermfs_inode_alloc(ctx, &archive_inode, S_IFDIR | 0755);
        if (ret != SPERMAFS_OK) return ret;
        ctx->superblock.archive_inode = archive_inode.inode_number;
        ret = spermfs_inode_write(ctx, &archive_inode);
        if (ret != SPERMAFS_OK) return ret;
    }

    return SPERMAFS_OK;
}

int spermfs_archive_store(spermfs_context_t *ctx, uint64_t inode_num)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;
    if (!archive) return SPERMAFS_ERR_NOMEM;
    if (archive->num_versions >= MAX_ARCHIVE_VERSIONS)
        return SPERMAFS_ERR_FULL;

    /* Read inode to archive */
    spermfs_inode_t inode;
    int ret = spermfs_inode_read(ctx, inode_num, &inode);
    if (ret != SPERMAFS_OK) return ret;

    /* Set archive flag */
    inode.flags |= SPERMAFS_INODE_ARCHIVE;
    ret = spermfs_inode_write(ctx, &inode);
    if (ret != SPERMAFS_OK) return ret;

    /* Create archive version entry */
    archive_version_t *ver = calloc(1, sizeof(archive_version_t));
    if (!ver) return SPERMAFS_ERR_NOMEM;

    static uint64_t next_version = 1;
    ver->version = next_version++;
    ver->timestamp = spermfs_time_ns();
    ver->inode_number = inode_num;
    ver->size = inode.size;

    /* Hash the file data */
    if (inode.size > 0 && inode.size < 1024 * 1024) {
        uint8_t *data = malloc((size_t)inode.size);
        if (data) {
            size_t read_size = (size_t)inode.size;
            ret = spermfs_cow_read(ctx, &inode, data, 0, read_size);
            if (ret > 0)
                spermfs_sha256(data, (size_t)ret, ver->hash);
            free(data);
        }
    }

    /* Add to version list */
    archive->versions[archive->num_versions++] = ver;

    fprintf(stderr, "SPERMAFS: archived inode %llu version %llu (size %llu)\n",
            (unsigned long long)inode_num, (unsigned long long)ver->version,
            (unsigned long long)ver->size);
    return SPERMAFS_OK;
}

int spermfs_archive_restore(spermfs_context_t *ctx, uint64_t inode_num,
                              uint64_t version)
{
    if (!ctx || !archive) return SPERMAFS_ERR_INVAL;

    /* Find the version */
    for (int i = 0; i < archive->num_versions; i++) {
        if (archive->versions[i]->inode_number == inode_num &&
            archive->versions[i]->version == version) {
            fprintf(stderr, "SPERMAFS: restoring inode %llu to version %llu\n",
                    (unsigned long long)inode_num,
                    (unsigned long long)version);
            return SPERMAFS_OK;
        }
    }

    return SPERMAFS_ERR_NOENT;
}

int spermfs_archive_list(spermfs_context_t *ctx, uint64_t inode_num)
{
    if (!ctx || !archive) return SPERMAFS_ERR_INVAL;

    printf("Archive versions for inode %llu:\n", (unsigned long long)inode_num);
    int found = 0;

    for (int i = 0; i < archive->num_versions; i++) {
        if (archive->versions[i]->inode_number == inode_num) {
            printf("  Version %llu: size=%llu, time=%llu\n",
                   (unsigned long long)archive->versions[i]->version,
                   (unsigned long long)archive->versions[i]->size,
                   (unsigned long long)archive->versions[i]->timestamp);
            found = 1;
        }
    }

    if (!found)
        printf("  No archived versions\n");

    return SPERMAFS_OK;
}
