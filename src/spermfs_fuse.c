#include "spermfs.h"
#include "fuse_compat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* Path -> inode resolution using B+Tree.
   The B+Tree maps (parent_inode << 32 | name_hash) -> child_inode */

static uint64_t path_hash(uint64_t parent, const char *name)
{
    uint64_t h = parent;
    while (*name)
        h = h * 31 + (unsigned char)*name++;
    return h;
}

/* Name component from hash (stored in value alongside inode) */
#define NAME_KEY(parent, name)  path_hash(parent, name)

static int resolve_path(spermfs_context_t *ctx, const char *path,
                         spermfs_inode_t *inode_out, uint64_t *parent_out)
{
    if (!ctx || !path) return -ENOENT;

    /* Start at root */
    uint64_t current_inode = ctx->superblock.root_inode;
    spermfs_inode_t inode;

    int ret = spermfs_inode_read(ctx, current_inode, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    if (strcmp(path, "/") == 0) {
        memcpy(inode_out, &inode, sizeof(spermfs_inode_t));
        if (parent_out) *parent_out = current_inode;
        return 0;
    }

    /* Skip leading / */
    while (*path == '/') path++;
    if (*path == '\0') {
        memcpy(inode_out, &inode, sizeof(spermfs_inode_t));
        if (parent_out) *parent_out = current_inode;
        return 0;
    }

    char component[SPERMAFS_MAX_NAME_LEN + 1];
    const char *p = path;

    while (*p) {
        /* Extract next component */
        int ci = 0;
        while (*p && *p != '/' && ci < SPERMAFS_MAX_NAME_LEN)
            component[ci++] = *p++;
        component[ci] = '\0';
        while (*p == '/') p++;

        /* Lookup in B+Tree */
        uint64_t hash_key = NAME_KEY(current_inode, component);
        uint64_t child_inode_num;

        ret = spermfs_btree_lookup(ctx, ctx->superblock.root_tree_root,
                                    hash_key, &child_inode_num);
        if (ret != SPERMAFS_OK) return -ENOENT;

        if (parent_out && *p == '\0')
            *parent_out = current_inode;

        current_inode = child_inode_num;

        ret = spermfs_inode_read(ctx, current_inode, &inode);
        if (ret != SPERMAFS_OK) return -EIO;
    }

    memcpy(inode_out, &inode, sizeof(spermfs_inode_t));
    return 0;
}

/* Create directory entry in B+Tree */
static int create_entry(spermfs_context_t *ctx, uint64_t parent_inode,
                         const char *name, uint64_t child_inode)
{
    uint64_t key = NAME_KEY(parent_inode, name);
    return spermfs_btree_insert(ctx, ctx->superblock.root_tree_root, key, child_inode);
}

/* Remove directory entry from B+Tree */
static int remove_entry(spermfs_context_t *ctx, uint64_t parent_inode,
                         const char *name)
{
    uint64_t key = NAME_KEY(parent_inode, name);
    return spermfs_btree_delete(ctx, ctx->superblock.root_tree_root, key);
}

/* Global context pointer for FUSE callbacks */
spermfs_context_t *g_ctx = NULL;

struct readdir_priv {
    uint64_t parent_inode;
    void *buf;
    int (*filler)(void *, const char *, const struct stat *, off_t, unsigned int);
    spermfs_context_t *ctx;
};

static int readdir_cb(uint64_t key, uint64_t val, void *priv)
{
    struct readdir_priv *rp = (struct readdir_priv *)priv;
    (void)key;
    spermfs_inode_t cinode;
    if (spermfs_inode_read(rp->ctx, val, &cinode) == SPERMAFS_OK) {
        char iname[64];
        snprintf(iname, sizeof(iname), "inode_%llu",
                 (unsigned long long)val);
        struct stat cst;
        memset(&cst, 0, sizeof(cst));
        cst.st_ino = val;
        cst.st_mode = (mode_t)cinode.mode;
        rp->filler(rp->buf, iname, &cst, 0, 0);
    }
    return 0;
}

/* ---- FUSE operations ---- */

static void *spermfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->use_ino = 1;
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    return g_ctx;
}

static int spermfs_getattr(const char *path, struct stat *st,
                            struct fuse_file_info *fi)
{
    (void)fi;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    memset(st, 0, sizeof(struct stat));
    st->st_ino = inode.inode_number;
    st->st_mode = (mode_t)inode.mode;
    st->st_nlink = inode.link_count;
    st->st_uid = inode.uid;
    st->st_gid = inode.gid;
    st->st_size = (off_t)inode.size;
    st->st_blksize = ctx->superblock.block_size;
    st->st_blocks = (inode.size + 511) / 512;
    st->st_atim.tv_sec = inode.atime / 1000000000ULL;
    st->st_mtim.tv_sec = inode.mtime / 1000000000ULL;
    st->st_ctim.tv_sec = inode.ctime / 1000000000ULL;
    st->st_atim.tv_nsec = inode.atime % 1000000000ULL;
    st->st_mtim.tv_nsec = inode.mtime % 1000000000ULL;
    st->st_ctim.tv_nsec = inode.ctime % 1000000000ULL;

    return 0;
}

static int spermfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)dev;
    spermfs_context_t *ctx = g_ctx;

    /* Extract parent and name */
    char parent_path[1024], name[SPERMAFS_MAX_NAME_LEN + 1];
    const char *slash = strrchr(path, '/');
    if (!slash) return -ENOENT;

    if (slash == path) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = slash - path;
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, slash + 1, SPERMAFS_MAX_NAME_LEN);
    name[SPERMAFS_MAX_NAME_LEN] = '\0';

    spermfs_inode_t parent;
    uint64_t parent_num;
    int ret = resolve_path(ctx, parent_path, &parent, &parent_num);
    if (ret != 0) return ret;

    if (!S_ISDIR(parent.mode)) return -ENOTDIR;

    /* Allocate new inode */
    spermfs_inode_t inode;
    ret = spermfs_inode_alloc(ctx, &inode, (uint64_t)mode);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Journal the operation */
    spermfs_journal_begin(ctx);
    spermfs_journal_log(ctx, inode.inode_number, 0, &inode, sizeof(inode));

    /* Write inode */
    ret = spermfs_inode_write(ctx, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Add to parent directory B+Tree */
    ret = create_entry(ctx, parent_num, name, inode.inode_number);
    if (ret != SPERMAFS_OK) return -EIO;

    spermfs_journal_commit(ctx);
    return 0;
}

static int spermfs_mkdir(const char *path, mode_t mode)
{
    return spermfs_mknod(path, S_IFDIR | mode, 0);
}

static int spermfs_unlink(const char *path)
{
    spermfs_context_t *ctx = g_ctx;

    char parent_path[1024], name[SPERMAFS_MAX_NAME_LEN + 1];
    const char *slash = strrchr(path, '/');
    if (!slash) return -ENOENT;

    if (slash == path) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = slash - path;
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, slash + 1, SPERMAFS_MAX_NAME_LEN);
    name[SPERMAFS_MAX_NAME_LEN] = '\0';

    spermfs_inode_t parent;
    uint64_t parent_num;
    int ret = resolve_path(ctx, parent_path, &parent, &parent_num);
    if (ret != 0) return ret;

    /* Resolve file to get inode number */
    spermfs_inode_t inode;
    ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    spermfs_journal_begin(ctx);
    spermfs_journal_log(ctx, inode.inode_number, 0, NULL, 0);

    /* Remove from B+Tree */
    ret = remove_entry(ctx, parent_num, name);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Free inode */
    ret = spermfs_inode_free(ctx, inode.inode_number);
    if (ret != SPERMAFS_OK) return -EIO;

    spermfs_journal_commit(ctx);
    return 0;
}

static int spermfs_rmdir(const char *path)
{
    return spermfs_unlink(path);
}

static int spermfs_rename(const char *from, const char *to, unsigned int flags)
{
    (void)flags;
    spermfs_context_t *ctx = g_ctx;

    char from_parent_path[1024], from_name[SPERMAFS_MAX_NAME_LEN + 1];
    const char *slash = strrchr(from, '/');
    if (!slash) return -ENOENT;
    if (slash == from) strcpy(from_parent_path, "/");
    else { memcpy(from_parent_path, from, slash - from); from_parent_path[slash - from] = '\0'; }
    strncpy(from_name, slash + 1, SPERMAFS_MAX_NAME_LEN);

    char to_parent_path[1024], to_name[SPERMAFS_MAX_NAME_LEN + 1];
    slash = strrchr(to, '/');
    if (!slash) return -ENOENT;
    if (slash == to) strcpy(to_parent_path, "/");
    else { memcpy(to_parent_path, to, slash - to); to_parent_path[slash - to] = '\0'; }
    strncpy(to_name, slash + 1, SPERMAFS_MAX_NAME_LEN);

    spermfs_inode_t from_parent, to_parent, inode;
    uint64_t from_parent_num, to_parent_num;

    int ret = resolve_path(ctx, from, &inode, NULL);
    if (ret != 0) return ret;
    ret = resolve_path(ctx, from_parent_path, &from_parent, &from_parent_num);
    if (ret != 0) return ret;
    ret = resolve_path(ctx, to_parent_path, &to_parent, &to_parent_num);
    if (ret != 0) return ret;

    spermfs_journal_begin(ctx);

    /* Remove from old parent */
    remove_entry(ctx, from_parent_num, from_name);

    /* Add to new parent */
    create_entry(ctx, to_parent_num, to_name, inode.inode_number);

    spermfs_journal_commit(ctx);
    return 0;
}

static int spermfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    inode.mode = (inode.mode & ~(uint64_t)07777) | (uint64_t)(mode & 07777);
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int spermfs_chown(const char *path, uid_t uid, gid_t gid,
                           struct fuse_file_info *fi)
{
    (void)fi;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    if (uid != (uid_t)-1) inode.uid = uid;
    if (gid != (gid_t)-1) inode.gid = gid;
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int spermfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    inode.size = (uint64_t)size;
    inode.mtime = spermfs_time_ns();
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int spermfs_open(const char *path, struct fuse_file_info *fi)
{
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    /* Store inode number in file handle */
    fi->fh = inode.inode_number;

    /* Track access for tiering */
    spermfs_tier_track_access(inode.inode_number,
                               spermfs_tier_select(ctx, inode.inode_number, 1));

    return 0;
}

static int spermfs_read(const char *path, char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = spermfs_inode_read(ctx, fi->fh, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Handle embedded data */
    if (inode.flags & SPERMAFS_INODE_EMBEDDED) {
        size_t avail = SPERMAFS_EMBEDDED_MAX;
        if (offset >= (off_t)avail) return 0;
        if ((size_t)offset + size > avail)
            size = avail - (size_t)offset;
        memcpy(buf, inode.embedded_data + offset, size);
        inode.atime = spermfs_time_ns();
        spermfs_inode_write(ctx, &inode);
        return (int)size;
    }

    /* Compressed read */
    if (inode.flags & SPERMAFS_INODE_COMPRESSED) {
        /* Read the compressed data, then decompress */
        /* For simplicity, fall through to cow_read */
    }

    ret = spermfs_cow_read(ctx, &inode, buf, (uint64_t)offset, size);

    inode.atime = spermfs_time_ns();
    spermfs_inode_write(ctx, &inode);

    return ret;
}

static int spermfs_write(const char *path, const char *buf, size_t size,
                           off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = spermfs_inode_read(ctx, fi->fh, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Check if file is small enough for embedded data */
    if (offset + (off_t)size <= SPERMAFS_EMBEDDED_MAX) {
        inode.flags |= SPERMAFS_INODE_EMBEDDED;
        memcpy(inode.embedded_data + offset, buf, size);
        if ((uint64_t)(offset + size) > inode.size)
            inode.size = (uint64_t)(offset + size);
        inode.mtime = spermfs_time_ns();
        ret = spermfs_inode_write(ctx, &inode);
        return (ret == SPERMAFS_OK) ? (int)size : -EIO;
    }

    inode.flags &= ~SPERMAFS_INODE_EMBEDDED;

    /* Dedup check */
    if (ctx->superblock.features & SPERMAFS_FEATURE_DEDUP) {
        uint8_t hash[32];
        uint64_t existing_block;
        if (spermfs_dedup_find(ctx, (const uint8_t *)buf, size, hash, &existing_block)
            == SPERMAFS_OK) {
            /* Dedup hit - reuse existing block */
            memcpy(inode.dedup_hash, hash, 32);
            inode.flags |= SPERMAFS_INODE_DEDUPED;
            spermfs_inode_add_extent(ctx, &inode, existing_block, 1);
            spermfs_dedup_ref(ctx, existing_block);
            inode.size = (uint64_t)(offset + size);
            inode.mtime = spermfs_time_ns();
            spermfs_inode_write(ctx, &inode);
            return (int)size;
        }
        /* No dedup hit, store hash for future dedup */
        spermfs_sha256((const uint8_t *)buf, size, hash);
    }

    /* COW write */
    spermfs_journal_begin(ctx);
    spermfs_journal_log(ctx, inode.inode_number, (uint64_t)offset, buf, size);

    ret = spermfs_cow_write(ctx, &inode, buf, (uint64_t)offset, size);
    if (ret != SPERMAFS_OK) {
        spermfs_journal_rollback(ctx);
        return -EIO;
    }

    /* Compression */
    if (ctx->superblock.features & SPERMAFS_FEATURE_COMPRESSION &&
        inode.size > ctx->superblock.block_size) {
        inode.flags |= SPERMAFS_INODE_COMPRESSED;
        inode.compression_algo = ctx->superblock.compression_algo;
    }

    /* Encryption */
    if (ctx->superblock.features & SPERMAFS_FEATURE_ENCRYPTION &&
        inode.encryption_algo == SPERMAFS_CRYPT_NONE) {
        inode.flags |= SPERMAFS_INODE_ENCRYPTED;
        inode.encryption_algo = ctx->superblock.encryption_algo;
    }

    ret = spermfs_inode_write(ctx, &inode);
    spermfs_journal_commit(ctx);

    return (ret == SPERMAFS_OK) ? (int)size : -EIO;
}

static int spermfs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int spermfs_fsync(const char *path, int isdatasync,
                           struct fuse_file_info *fi)
{
    (void)path;
    (void)isdatasync;
    (void)fi;
    spermfs_journal_commit(g_ctx);
    return 0;
}

static int spermfs_opendir(const char *path, struct fuse_file_info *fi)
{
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;
    if (!S_ISDIR(inode.mode)) return -ENOTDIR;

    fi->fh = inode.inode_number;
    return 0;
}

static int spermfs_readdir(const char *path, void *buf,
                             int (*filler)(void *, const char *, const struct stat *,
                                           off_t, unsigned int),
                             off_t offset, struct fuse_file_info *fi,
                             unsigned int flags)
{
    (void)offset;
    (void)flags;
    spermfs_context_t *ctx = g_ctx;

    (void)fi;
    spermfs_inode_t dir_inode;
    int ret = resolve_path(ctx, path, &dir_inode, NULL);
    if (ret != 0) return ret;

    /* Always add . and .. */
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = dir_inode.inode_number;
    st.st_mode = S_IFDIR | 0755;
    filler(buf, ".", &st, 0, 0);

    if (strcmp(path, "/") == 0) {
        st.st_ino = ctx->superblock.root_inode;
    } else {
        spermfs_inode_t parent;
        uint64_t parent_num;
        resolve_path(ctx, path, &parent, &parent_num);
        st.st_ino = parent_num;
    }
    filler(buf, "..", &st, 0, 0);

    struct readdir_priv priv;
    priv.parent_inode = dir_inode.inode_number;
    priv.buf = buf;
    priv.filler = filler;
    priv.ctx = ctx;

    spermfs_btree_iterate(ctx, ctx->superblock.root_tree_root,
                           readdir_cb, &priv);

    return 0;
}

static int spermfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int spermfs_access(const char *path, int mask)
{
    (void)mask;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    return resolve_path(ctx, path, &inode, NULL);
}

static int spermfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int ret = spermfs_mknod(path, mode | S_IFREG, 0);
    if (ret != 0) return ret;

    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    fi->fh = inode.inode_number;
    return 0;
}

static int spermfs_utimens(const char *path, const struct timespec tv[2],
                             struct fuse_file_info *fi)
{
    (void)fi;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(ctx, path, &inode, NULL);
    if (ret != 0) return ret;

    inode.atime = (uint64_t)tv[0].tv_sec * 1000000000ULL + (uint64_t)tv[0].tv_nsec;
    inode.mtime = (uint64_t)tv[1].tv_sec * 1000000000ULL + (uint64_t)tv[1].tv_nsec;

    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int spermfs_statfs(const char *path, struct statvfs *stfs)
{
    spermfs_context_t *ctx = g_ctx;

    memset(stfs, 0, sizeof(struct statvfs));
    stfs->f_bsize = ctx->superblock.block_size;
    stfs->f_frsize = ctx->superblock.block_size;
    stfs->f_blocks = ctx->superblock.total_blocks;
    stfs->f_bfree = ctx->superblock.total_blocks - ctx->superblock.used_blocks;
    stfs->f_bavail = stfs->f_bfree;
    stfs->f_files = ctx->superblock.next_inode;
    stfs->f_ffree = 0;
    stfs->f_namemax = SPERMAFS_MAX_NAME_LEN;

    return 0;
}

const struct fuse_operations spermfs_ops = {
    .getattr    = spermfs_getattr,
    .readlink   = NULL,
    .mknod      = spermfs_mknod,
    .mkdir      = spermfs_mkdir,
    .unlink     = spermfs_unlink,
    .rmdir      = spermfs_rmdir,
    .symlink    = NULL,
    .rename     = spermfs_rename,
    .link       = NULL,
    .chmod      = spermfs_chmod,
    .chown      = spermfs_chown,
    .truncate   = spermfs_truncate,
    .open       = spermfs_open,
    .read       = spermfs_read,
    .write      = spermfs_write,
    .statfs     = spermfs_statfs,
    .flush      = NULL,
    .release    = spermfs_release,
    .fsync      = spermfs_fsync,
    .setxattr   = NULL,
    .getxattr   = NULL,
    .listxattr  = NULL,
    .removexattr = NULL,
    .opendir    = spermfs_opendir,
    .readdir    = spermfs_readdir,
    .releasedir = spermfs_releasedir,
    .fsyncdir   = NULL,
    .init       = spermfs_init,
    .destroy    = NULL,
    .access     = spermfs_access,
    .create     = spermfs_create,
    .lock       = NULL,
    .utimens    = spermfs_utimens,
    .bmap       = NULL,
    .ioctl      = NULL,
    .poll       = NULL,
    .write_buf  = NULL,
    .read_buf   = NULL,
    .flock      = NULL,
    .fallocate  = NULL,
    .copy_file_range = NULL,
    .lseek      = NULL,
};
