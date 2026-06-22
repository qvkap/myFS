#ifndef FUSE_PROTOCOL_H
#define FUSE_PROTOCOL_H

#include <linux/fuse.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

/* Minimal FUSE protocol implementation using /dev/fuse directly. */

#define FUSE_BUFSIZE  (256 * 1024)
#define FUSE_MAX_WRITE (128 * 1024)

/* Minimal file info struct (like libfuse's fuse_file_info) */
struct fuse_file_info {
    int flags;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    unsigned int flush : 1;
    unsigned int nonseekable : 1;
    unsigned int flock_release : 1;
    unsigned int cache_readdir : 1;
    uint64_t fh;
    uint64_t lock_owner;
    uint64_t poll_events;
};

struct fuse_dev_ctx {
    int     fd;
    char    mountpoint[1024];
    int     debug;
    int     running;
    uint64_t unique;
    void    *user_data;

    /* Receive buffer */
    char    buf[FUSE_BUFSIZE];
    size_t  buf_len;
};

/* FUSE operations (called from protocol handler) */
struct fuse_dev_ops {
    int (*getattr)(const char *path, struct stat *st, void *user_data);
    int (*readlink)(const char *path, char *buf, size_t size, void *user_data);
    int (*mknod)(const char *path, mode_t mode, dev_t dev, void *user_data);
    int (*mkdir)(const char *path, mode_t mode, void *user_data);
    int (*unlink)(const char *path, void *user_data);
    int (*rmdir)(const char *path, void *user_data);
    int (*symlink)(const char *from, const char *to, void *user_data);
    int (*rename)(const char *from, const char *to, unsigned int flags, void *user_data);
    int (*link)(const char *from, const char *to, void *user_data);
    int (*chmod)(const char *path, mode_t mode, void *user_data);
    int (*chown)(const char *path, uid_t uid, gid_t gid, void *user_data);
    int (*truncate)(const char *path, off_t size, void *user_data);
    int (*open)(const char *path, struct fuse_file_info *fi, void *user_data);
    int (*read)(const char *path, char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi, void *user_data);
    int (*write)(const char *path, const char *buf, size_t size, off_t offset,
                 struct fuse_file_info *fi, void *user_data);
    int (*statfs)(const char *path, struct statvfs *st, void *user_data);
    int (*flush)(const char *path, struct fuse_file_info *fi, void *user_data);
    int (*release)(const char *path, struct fuse_file_info *fi, void *user_data);
    int (*fsync)(const char *path, int datasync, struct fuse_file_info *fi, void *user_data);
    int (*opendir)(const char *path, struct fuse_file_info *fi, void *user_data);
    int (*readdir)(const char *path, void *buf,
                   int (*filler)(void *, const char *, const struct stat *, off_t),
                   off_t offset, struct fuse_file_info *fi, void *user_data);
    int (*releasedir)(const char *path, struct fuse_file_info *fi, void *user_data);
    int (*access)(const char *path, int mask, void *user_data);
    int (*create)(const char *path, mode_t mode, struct fuse_file_info *fi, void *user_data);
    int (*utimens)(const char *path, const struct timespec tv[2], void *user_data);
    int (*init)(void *user_data);
    void (*destroy)(void *user_data);
};

int fuse_dev_mount(struct fuse_dev_ctx *ctx, const char *mountpoint);
int fuse_dev_loop(struct fuse_dev_ctx *ctx, const struct fuse_dev_ops *ops);
void fuse_dev_unmount(struct fuse_dev_ctx *ctx);

/* Resolve inode number to full path (implemented in fuse_bridge.c) */
int fuse_bridge_inode_to_path(uint64_t inode_num, char *path, size_t path_len);

#endif /* FUSE_PROTOCOL_H */
