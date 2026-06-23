#include "spermfs.h"
#include "fuse_protocol.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* Bridge between SPERMAFS internal API and FUSE protocol callbacks */

extern spermfs_context_t *g_ctx;

/* Path -> inode resolution using B+Tree */
static uint64_t path_hash(uint64_t parent, const char *name)
{
    uint64_t h = parent;
    while (*name)
        h = h * 31 + (unsigned char)*name++;
    return h;
}

#define NAME_KEY(parent, name)  path_hash(parent, name)

static int resolve_path(const char *path, spermfs_inode_t *inode_out,
                         uint64_t *parent_out)
{
    spermfs_context_t *ctx = g_ctx;
    if (!ctx || !path) return -ENOENT;

    uint64_t current_inode = ctx->superblock.root_inode;
    spermfs_inode_t inode;

    int ret = spermfs_inode_read(ctx, current_inode, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    if (strcmp(path, "/") == 0) {
        if (inode_out) memcpy(inode_out, &inode, sizeof(spermfs_inode_t));
        if (parent_out) *parent_out = current_inode;
        return 0;
    }

    while (*path == '/') path++;
    if (*path == '\0') {
        if (inode_out) memcpy(inode_out, &inode, sizeof(spermfs_inode_t));
        if (parent_out) *parent_out = current_inode;
        return 0;
    }

    char component[256];
    const char *p = path;

    while (*p) {
        int ci = 0;
        while (*p && *p != '/' && ci < 255)
            component[ci++] = *p++;
        component[ci] = '\0';
        while (*p == '/') p++;

        uint64_t child_inode_num;
        ret = spermfs_btree_lookup(ctx, ctx->superblock.root_tree_root,
                                    NAME_KEY(current_inode, component),
                                    &child_inode_num);
        if (ret != SPERMAFS_OK) return -ENOENT;

        if (parent_out && *p == '\0')
            *parent_out = current_inode;

        current_inode = child_inode_num;

        ret = spermfs_inode_read(ctx, current_inode, &inode);
        if (ret != SPERMAFS_OK) return -EIO;
    }

    if (inode_out) memcpy(inode_out, &inode, sizeof(spermfs_inode_t));
    return 0;
}

static int create_entry(uint64_t parent_inode, const char *name,
                         uint64_t child_inode)
{
    spermfs_context_t *ctx = g_ctx;
    uint64_t key = NAME_KEY(parent_inode, name);
    return spermfs_btree_insert(ctx, ctx->superblock.root_tree_root, key, child_inode);
}

static int remove_entry(uint64_t parent_inode, const char *name)
{
    spermfs_context_t *ctx = g_ctx;
    uint64_t key = NAME_KEY(parent_inode, name);
    return spermfs_btree_delete(ctx, ctx->superblock.root_tree_root, key);
}

static int inode_to_path(uint64_t inode_num, char *path, size_t path_len)
{
    spermfs_context_t *ctx = g_ctx;
    if (!ctx || !path || path_len == 0) return -ENOENT;

    uint64_t root_ino = ctx->superblock.root_inode;

    if (inode_num == root_ino) {
        snprintf(path, path_len, "/");
        return 0;
    }

    char components[256][SPERMAFS_MAX_NAME_LEN];
    int ncomps = 0;

    while (inode_num != root_ino) {
        spermfs_inode_t inode;
        int ret = spermfs_inode_read(ctx, inode_num, &inode);
        if (ret != SPERMAFS_OK) return -EIO;

        if (ncomps >= 256) return -ENAMETOOLONG;

        strncpy(components[ncomps], inode.name, SPERMAFS_MAX_NAME_LEN - 1);
        components[ncomps][SPERMAFS_MAX_NAME_LEN - 1] = '\0';
        ncomps++;

        if (inode.parent_inode == 0 || inode.parent_inode == inode_num)
            break;

        inode_num = inode.parent_inode;
    }

    size_t off = 0;
    path[off++] = '/';

    for (int i = ncomps - 1; i >= 0; i--) {
        size_t nlen = strlen(components[i]);
        if (off + nlen + 1 > path_len) return -ENAMETOOLONG;
        memcpy(path + off, components[i], nlen);
        off += nlen;
        if (i > 0) path[off++] = '/';
    }

    path[off] = '\0';
    return 0;
}

int fuse_bridge_inode_to_path(uint64_t inode_num, char *path, size_t path_len)
{
    spermfs_context_t *ctx = g_ctx;
    if (inode_num == FUSE_ROOT_ID)
        inode_num = ctx->superblock.root_inode;
    return inode_to_path(inode_num, path, path_len);
}

/* ---- FUSE callback implementations ---- */

static int bridge_getattr(const char *path, struct stat *st, void *user_data)
{
    (void)user_data;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;

    memset(st, 0, sizeof(struct stat));
    st->st_ino = inode.inode_number;
    st->st_mode = (mode_t)inode.mode;
    st->st_nlink = inode.link_count;
    st->st_uid = inode.uid;
    st->st_gid = inode.gid;
    st->st_size = (off_t)inode.size;
    st->st_blksize = g_ctx->superblock.block_size;
    st->st_blocks = (inode.size + 511) / 512;
    st->st_atim.tv_sec = inode.atime / 1000000000ULL;
    st->st_mtim.tv_sec = inode.mtime / 1000000000ULL;
    st->st_ctim.tv_sec = inode.ctime / 1000000000ULL;
    st->st_atim.tv_nsec = inode.atime % 1000000000ULL;
    st->st_mtim.tv_nsec = inode.mtime % 1000000000ULL;
    st->st_ctim.tv_nsec = inode.ctime % 1000000000ULL;

    return 0;
}

static int bridge_mknod(const char *path, mode_t mode, dev_t dev, void *user_data)
{
    (void)dev;
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;

    /* Extract parent and name */
    char parent_path[1024], name[256];
    const char *slash = strrchr(path, '/');
    if (!slash) { fprintf(stderr, "SPERMAFS_DBG: no slash in '%s'\n", path); return -ENOENT; }

    if (slash == path) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = slash - path;
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, slash + 1, 255);
    name[255] = '\0';

    spermfs_inode_t parent;
    uint64_t parent_num;
    int ret = resolve_path(parent_path, &parent, &parent_num);
    if (ret != 0) return ret;

    spermfs_inode_t inode;
    ret = spermfs_inode_alloc(ctx, &inode, (uint64_t)mode);
    if (ret != SPERMAFS_OK) return -EIO;

    inode.parent_inode = parent.inode_number;
    strncpy(inode.name, name, SPERMAFS_MAX_NAME_LEN - 1);
    inode.name[SPERMAFS_MAX_NAME_LEN - 1] = '\0';

    spermfs_journal_begin(ctx);
    spermfs_journal_log(ctx, inode.inode_number, 0, &inode, sizeof(spermfs_inode_t));

    ret = spermfs_inode_write(ctx, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    ret = create_entry(parent.inode_number, name, inode.inode_number);
    if (ret != SPERMAFS_OK) return -EIO;

    spermfs_journal_commit(ctx);
    return 0;
}

static int bridge_mkdir(const char *path, mode_t mode, void *user_data)
{
    return bridge_mknod(path, S_IFDIR | mode, 0, user_data);
}

static int bridge_unlink(const char *path, void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;

    char parent_path[1024], name[256];
    const char *slash = strrchr(path, '/');
    if (!slash) return -ENOENT;

    if (slash == path) strcpy(parent_path, "/");
    else { memcpy(parent_path, path, slash - path); parent_path[slash - path] = '\0'; }
    strncpy(name, slash + 1, 255);

    spermfs_inode_t parent;
    uint64_t parent_num;
    int ret = resolve_path(parent_path, &parent, &parent_num);
    if (ret != 0) return ret;

    spermfs_inode_t inode;
    ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;

    spermfs_journal_begin(ctx);
    remove_entry(parent.inode_number, name);
    if (inode.link_count > 1) {
        inode.link_count--;
        spermfs_inode_write(ctx, &inode);
    } else {
        spermfs_inode_free(ctx, inode.inode_number);
    }
    spermfs_journal_commit(ctx);
    return 0;
}

static int bridge_rmdir(const char *path, void *user_data)
{
    return bridge_unlink(path, user_data);
}

static int bridge_rename(const char *from, const char *to, unsigned int flags,
                          void *user_data)
{
    (void)flags;
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;

    char from_parent[1024], from_name[256];
    const char *slash = strrchr(from, '/');
    if (!slash) return -ENOENT;
    if (slash == from) strcpy(from_parent, "/");
    else { memcpy(from_parent, from, slash - from); from_parent[slash - from] = '\0'; }
    strncpy(from_name, slash + 1, 255);

    char to_parent[1024], to_name[256];
    slash = strrchr(to, '/');
    if (!slash) return -ENOENT;
    if (slash == to) strcpy(to_parent, "/");
    else { memcpy(to_parent, to, slash - to); to_parent[slash - to] = '\0'; }
    strncpy(to_name, slash + 1, 255);

    uint64_t from_parent_num, to_parent_num;
    spermfs_inode_t inode, from_parent_inode, to_parent_inode;
    int ret = resolve_path(from, &inode, NULL);
    if (ret != 0) return ret;
    ret = resolve_path(from_parent, &from_parent_inode, &from_parent_num);
    if (ret != 0) return ret;
    ret = resolve_path(to_parent, &to_parent_inode, &to_parent_num);
    if (ret != 0) return ret;

    spermfs_journal_begin(ctx);
    remove_entry(from_parent_inode.inode_number, from_name);
    create_entry(to_parent_inode.inode_number, to_name, inode.inode_number);
    inode.parent_inode = to_parent_inode.inode_number;
    strncpy(inode.name, to_name, SPERMAFS_MAX_NAME_LEN - 1);
    inode.name[SPERMAFS_MAX_NAME_LEN - 1] = '\0';
    spermfs_inode_write(ctx, &inode);
    spermfs_journal_commit(ctx);
    return 0;
}

static int bridge_truncate(const char *path, off_t size, void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;
    inode.size = (uint64_t)size;
    inode.mtime = spermfs_time_ns();
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int bridge_open(const char *path, struct fuse_file_info *fi, void *user_data)
{
    (void)user_data;
    (void)g_ctx;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;
    fi->fh = inode.inode_number;
    return 0;
}

static int bridge_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi, void *user_data)
{
    (void)path;
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    int ret = spermfs_inode_read(ctx, fi->fh, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Embedded data */
    if (inode.flags & SPERMAFS_INODE_EMBEDDED) {
        size_t avail = SPERMAFS_EMBEDDED_MAX;
        if (offset >= (off_t)avail) return 0;
        if ((size_t)offset + size > avail)
            size = avail - (size_t)offset;
        memcpy(buf, inode.embedded_data + offset, size);
        return (int)size;
    }

    ret = spermfs_cow_read(ctx, &inode, buf, (uint64_t)offset, size);
    inode.atime = spermfs_time_ns();
    spermfs_inode_write(ctx, &inode);
    return ret;
}

static int bridge_write(const char *path, const char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi, void *user_data)
{
    (void)path;
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    int ret = spermfs_inode_read(ctx, fi->fh, &inode);
    if (ret != SPERMAFS_OK) return -EIO;

    /* Embedded data path for small files */
    if (offset + (off_t)size <= SPERMAFS_EMBEDDED_MAX) {
        inode.flags |= SPERMAFS_INODE_EMBEDDED;
        memcpy(inode.embedded_data + offset, buf, size);
        if ((uint64_t)(offset + size) > inode.size)
            inode.size = (uint64_t)(offset + size);
        inode.mtime = spermfs_time_ns();
        return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? (int)size : -EIO;
    }

    inode.flags &= ~SPERMAFS_INODE_EMBEDDED;

    spermfs_journal_begin(ctx);
    spermfs_journal_log(ctx, inode.inode_number, (uint64_t)offset, buf, size);

    ret = spermfs_cow_write(ctx, &inode, buf, (uint64_t)offset, size);
    if (ret != SPERMAFS_OK) {
        spermfs_journal_rollback(ctx);
        return -EIO;
    }

    ret = spermfs_inode_write(ctx, &inode);
    spermfs_journal_commit(ctx);
    return (ret == SPERMAFS_OK) ? (int)size : -EIO;
}

static int bridge_flush(const char *path, struct fuse_file_info *fi, void *user_data)
{
    (void)path;
    (void)fi;
    (void)user_data;
    return 0;
}

static int bridge_release(const char *path, struct fuse_file_info *fi, void *user_data)
{
    (void)path;
    (void)fi;
    (void)user_data;
    return 0;
}

static int bridge_fsync(const char *path, int datasync, struct fuse_file_info *fi,
                         void *user_data)
{
    (void)path;
    (void)datasync;
    (void)fi;
    (void)user_data;
    spermfs_journal_commit(g_ctx);
    return 0;
}

static int bridge_opendir(const char *path, struct fuse_file_info *fi, void *user_data)
{
    (void)user_data;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;
    fi->fh = inode.inode_number;
    return 0;
}

struct fill_dir_priv {
    void *buf;
    int (*filler)(void *, const char *, const struct stat *, off_t);
    uint64_t parent_inode;
    off_t min_offset;
    off_t entry_index;
};

static int fill_dir_cb(uint64_t key, uint64_t val, void *priv)
{
    struct fill_dir_priv *fp = (struct fill_dir_priv *)priv;
    (void)key;
    spermfs_inode_t cinode;
    if (spermfs_inode_read(g_ctx, val, &cinode) != SPERMAFS_OK)
        return 0;

    /* Only show entries that belong to this directory */
    if (cinode.parent_inode != fp->parent_inode)
        return 0;

    fp->entry_index++;
    if (fp->entry_index < fp->min_offset)
        return 0;

    const char *display_name = cinode.name[0] ? cinode.name : "?";
    struct stat cst;
    memset(&cst, 0, sizeof(cst));
    cst.st_ino = val;
    cst.st_mode = (mode_t)cinode.mode;
    fp->filler(fp->buf, display_name, &cst, fp->entry_index);
    return 0;
}

static int bridge_readdir(const char *path, void *buf,
                            int (*filler)(void *, const char *, const struct stat *, off_t),
                            off_t offset, struct fuse_file_info *fi, void *user_data)
{
    (void)fi;
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t dir_inode;
    int ret = resolve_path(path, &dir_inode, NULL);
    if (ret != 0) return ret;

    struct fill_dir_priv priv;
    priv.buf = buf;
    priv.filler = filler;
    priv.parent_inode = dir_inode.inode_number;
    priv.min_offset = offset > 0 ? offset : 0;
    priv.entry_index = 0;

    /* . and .. only at the beginning */
    if (offset == 0) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = dir_inode.inode_number;
        st.st_mode = S_IFDIR | 0755;
        priv.entry_index = 1;
        filler(buf, ".", &st, 1);

        if (strcmp(path, "/") == 0)
            st.st_ino = ctx->superblock.root_inode;
        else
            st.st_ino = dir_inode.parent_inode;
        priv.entry_index = 2;
        filler(buf, "..", &st, 2);
    }

    spermfs_btree_iterate(ctx, ctx->superblock.root_tree_root, fill_dir_cb, &priv);
    return 0;
}

static int bridge_releasedir(const char *path, struct fuse_file_info *fi,
                              void *user_data)
{
    (void)path;
    (void)fi;
    (void)user_data;
    return 0;
}

static int bridge_access(const char *path, int mask, void *user_data)
{
    (void)mask;
    (void)user_data;
    spermfs_inode_t inode;
    return resolve_path(path, &inode, NULL);
}

static int bridge_create(const char *path, mode_t mode, struct fuse_file_info *fi,
                          void *user_data)
{
    int ret = bridge_mknod(path, mode | S_IFREG, 0, user_data);
    if (ret != 0) { fprintf(stderr, "SPERMAFS_DBG: bridge_mknod ret=%d\n", ret); return ret; }

    spermfs_inode_t inode;
    ret = resolve_path(path, &inode, NULL);
    if (ret != 0) { fprintf(stderr, "SPERMAFS_DBG: resolve after create failed %d\n", ret); return ret; }
    fi->fh = inode.inode_number;
    fprintf(stderr, "SPERMAFS_DBG: bridge_create OK fh=%llu\n", (unsigned long long)fi->fh);
    return 0;
}

static int bridge_utimens(const char *path, const struct timespec tv[2],
                           void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;
    inode.atime = (uint64_t)tv[0].tv_sec * 1000000000ULL + (uint64_t)tv[0].tv_nsec;
    inode.mtime = (uint64_t)tv[1].tv_sec * 1000000000ULL + (uint64_t)tv[1].tv_nsec;
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int bridge_statfs(const char *path, struct statvfs *st, void *user_data)
{
    (void)path;
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    memset(st, 0, sizeof(*st));
    st->f_bsize = ctx->superblock.block_size;
    st->f_frsize = ctx->superblock.block_size;
    st->f_blocks = ctx->superblock.total_blocks;
    st->f_bfree = ctx->superblock.total_blocks - ctx->superblock.used_blocks;
    st->f_bavail = st->f_bfree;
    st->f_files = ctx->superblock.next_inode;
    st->f_ffree = 0;
    st->f_namemax = 255;
    return 0;
}

static int bridge_init(void *user_data)
{
    (void)user_data;
    return 0;
}

static void bridge_destroy(void *user_data)
{
    (void)user_data;
}

static int bridge_chmod(const char *path, mode_t mode, void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;
    inode.mode = mode;
    inode.ctime = spermfs_time_ns();
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int bridge_chown(const char *path, uid_t uid, gid_t gid, void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;
    inode.uid = uid;
    inode.gid = gid;
    inode.ctime = spermfs_time_ns();
    return spermfs_inode_write(ctx, &inode) == SPERMAFS_OK ? 0 : -EIO;
}

static int bridge_symlink(const char *from, const char *to, void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;

    char parent_path[1024], name[256];
    const char *slash = strrchr(to, '/');
    if (!slash) return -ENOENT;
    if (slash == to) strcpy(parent_path, "/");
    else { memcpy(parent_path, to, slash - to); parent_path[slash - to] = '\0'; }
    strncpy(name, slash + 1, 255);
    name[255] = '\0';

    spermfs_inode_t parent;
    int ret = resolve_path(parent_path, &parent, NULL);
    if (ret != 0) return ret;

    spermfs_inode_t inode;
    ret = spermfs_inode_alloc(ctx, &inode, S_IFLNK | 0777);
    if (ret != SPERMAFS_OK) return -EIO;

    inode.parent_inode = parent.inode_number;
    strncpy(inode.name, name, SPERMAFS_MAX_NAME_LEN - 1);
    inode.name[SPERMAFS_MAX_NAME_LEN - 1] = '\0';

    size_t target_len = strlen(from);
    if (target_len < SPERMAFS_EMBEDDED_MAX) {
        inode.flags |= SPERMAFS_INODE_EMBEDDED;
        memcpy(inode.embedded_data, from, target_len);
    }
    inode.size = target_len;

    spermfs_journal_begin(ctx);
    ret = spermfs_inode_write(ctx, &inode);
    if (ret != SPERMAFS_OK) return -EIO;
    ret = create_entry(parent.inode_number, name, inode.inode_number);
    if (ret != SPERMAFS_OK) return -EIO;
    spermfs_journal_commit(ctx);
    return 0;
}

static int bridge_readlink(const char *path, char *buf, size_t size, void *user_data)
{
    (void)user_data;
    spermfs_inode_t inode;
    int ret = resolve_path(path, &inode, NULL);
    if (ret != 0) return ret;

    size_t len = inode.size;
    if (len > size) len = size;
    if (inode.flags & SPERMAFS_INODE_EMBEDDED)
        memcpy(buf, inode.embedded_data, len);
    buf[len] = '\0';
    return 0;
}

static int bridge_link(const char *from, const char *to, void *user_data)
{
    (void)user_data;
    spermfs_context_t *ctx = g_ctx;

    spermfs_inode_t inode;
    int ret = resolve_path(from, &inode, NULL);
    if (ret != 0) return ret;

    char parent_path[1024], name[256];
    const char *slash = strrchr(to, '/');
    if (!slash) return -ENOENT;
    if (slash == to) strcpy(parent_path, "/");
    else { memcpy(parent_path, to, slash - to); parent_path[slash - to] = '\0'; }
    strncpy(name, slash + 1, 255);
    name[255] = '\0';

    spermfs_inode_t parent;
    ret = resolve_path(parent_path, &parent, NULL);
    if (ret != 0) return ret;

    inode.link_count++;

    spermfs_journal_begin(ctx);
    ret = spermfs_inode_write(ctx, &inode);
    if (ret != SPERMAFS_OK) return -EIO;
    ret = create_entry(parent.inode_number, name, inode.inode_number);
    if (ret != SPERMAFS_OK) return -EIO;
    spermfs_journal_commit(ctx);
    return 0;
}

static const struct fuse_dev_ops bridge_ops = {
    .getattr   = bridge_getattr,
    .readlink  = bridge_readlink,
    .mknod     = bridge_mknod,
    .mkdir     = bridge_mkdir,
    .unlink    = bridge_unlink,
    .rmdir     = bridge_rmdir,
    .symlink   = bridge_symlink,
    .rename    = bridge_rename,
    .link      = bridge_link,
    .chmod     = bridge_chmod,
    .chown     = bridge_chown,
    .truncate  = bridge_truncate,
    .open      = bridge_open,
    .read      = bridge_read,
    .write     = bridge_write,
    .statfs    = bridge_statfs,
    .flush     = bridge_flush,
    .release   = bridge_release,
    .fsync     = bridge_fsync,
    .opendir   = bridge_opendir,
    .readdir   = bridge_readdir,
    .releasedir = bridge_releasedir,
    .access    = bridge_access,
    .create    = bridge_create,
    .utimens   = bridge_utimens,
    .init      = bridge_init,
    .destroy   = bridge_destroy,
};

int fuse_bridge_main(spermfs_context_t *ctx, int argc, char **argv)
{
    struct fuse_dev_ctx fdev;
    memset(&fdev, 0, sizeof(fdev));
    fdev.user_data = ctx;
    fdev.debug = 0;

    /* Find mountpoint in argv */
    const char *mountpoint = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            if (!mountpoint) {
                /* Skip device arg if present */
                if (i == 1 && argc > 2) continue;
                mountpoint = argv[i];
            }
        }
        if (strcmp(argv[i], "-d") == 0) fdev.debug = 1;
    }

    if (!mountpoint) {
        fprintf(stderr, "SPERMAFS: no mountpoint specified\n");
        return 1;
    }

    int ret = fuse_dev_mount(&fdev, mountpoint);
    if (ret != 0) return 1;

    ret = fuse_dev_loop(&fdev, &bridge_ops);
    fuse_dev_unmount(&fdev);
    return ret;
}
