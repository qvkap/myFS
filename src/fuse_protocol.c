#include "fuse_protocol.h"
#include <linux/fuse.h>
#include <linux/mount.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>

/* FUSE protocol implementation using /dev/fuse directly */

#define FUSE_DEVICE "/dev/fuse"
#define FUSE_MAJOR_VER 7
#define FUSE_MINOR_VER 42

static void fill_attr(struct fuse_attr *fa, const struct stat *st)
{
    memset(fa, 0, sizeof(*fa));
    fa->ino = st->st_ino;
    fa->size = st->st_size;
    fa->blocks = st->st_blocks;
    fa->atime = st->st_atim.tv_sec;
    fa->mtime = st->st_mtim.tv_sec;
    fa->ctime = st->st_ctim.tv_sec;
    fa->atimensec = st->st_atim.tv_nsec;
    fa->mtimensec = st->st_mtim.tv_nsec;
    fa->ctimensec = st->st_ctim.tv_nsec;
    fa->mode = st->st_mode;
    fa->nlink = st->st_nlink;
    fa->uid = st->st_uid;
    fa->gid = st->st_gid;
    fa->blksize = st->st_blksize;
}

static int send_reply(struct fuse_dev_ctx *ctx, struct fuse_out_header *out, size_t size)
{
    if (write(ctx->fd, out, size) != (ssize_t)size) {
        perror("SPERMAFS: write to fuse device");
        return -1;
    }
    return 0;
}

static int reply_err(struct fuse_dev_ctx *ctx, uint64_t unique, int error)
{
    struct fuse_out_header out = {
        .len = sizeof(struct fuse_out_header),
        .error = -error,
        .unique = unique,
    };
    return send_reply(ctx, &out, sizeof(out));
}

static int reply_attr(struct fuse_dev_ctx *ctx, uint64_t unique, const struct stat *st, double timeout)
{
    char buf[sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out)];
    struct fuse_out_header *out = (struct fuse_out_header *)buf;
    struct fuse_attr_out *attr = (struct fuse_attr_out *)(buf + sizeof(*out));

    memset(buf, 0, sizeof(buf));
    out->len = sizeof(buf);
    out->unique = unique;
    attr->attr_valid = (uint64_t)(timeout * 1e9);
    fill_attr(&attr->attr, st);

    return send_reply(ctx, out, sizeof(buf));
}

static int reply_entry(struct fuse_dev_ctx *ctx, uint64_t unique,
                        const struct stat *st, double timeout, uint64_t nodeid,
                        int *generation)
{
    char buf[sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out)];
    struct fuse_out_header *out = (struct fuse_out_header *)buf;
    struct fuse_entry_out *entry = (struct fuse_entry_out *)(buf + sizeof(*out));

    memset(buf, 0, sizeof(buf));
    out->len = sizeof(buf);
    out->unique = unique;
    entry->nodeid = nodeid;
    entry->generation = generation ? *generation : 0;
    entry->entry_valid = (uint64_t)(timeout * 1e9);
    entry->attr_valid = (uint64_t)(timeout * 1e9);
    fill_attr(&entry->attr, st);

    return send_reply(ctx, out, sizeof(buf));
}

static int reply_open(struct fuse_dev_ctx *ctx, uint64_t unique, uint64_t fh)
{
    char buf[sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out)];
    struct fuse_out_header *out = (struct fuse_out_header *)buf;
    struct fuse_open_out *open_out = (struct fuse_open_out *)(buf + sizeof(*out));

    memset(buf, 0, sizeof(buf));
    out->len = sizeof(buf);
    out->unique = unique;
    open_out->fh = fh;
    open_out->open_flags = 0;

    return send_reply(ctx, out, sizeof(buf));
}

static int reply_buf(struct fuse_dev_ctx *ctx, uint64_t unique, const char *buf_data, size_t size)
{
    size_t total = sizeof(struct fuse_out_header) + size;
    char *reply = malloc(total);
    if (!reply) return reply_err(ctx, unique, ENOMEM);

    struct fuse_out_header *out = (struct fuse_out_header *)reply;
    out->len = total;
    out->unique = unique;
    out->error = 0;
    if (buf_data && size > 0)
        memcpy(reply + sizeof(*out), buf_data, size);

    int ret = send_reply(ctx, out, total);
    free(reply);
    return ret;
}

static int reply_write(struct fuse_dev_ctx *ctx, uint64_t unique, uint32_t count)
{
    char buf[sizeof(struct fuse_out_header) + sizeof(struct fuse_write_out)];
    struct fuse_out_header *out = (struct fuse_out_header *)buf;
    struct fuse_write_out *wout = (struct fuse_write_out *)(buf + sizeof(*out));

    memset(buf, 0, sizeof(buf));
    out->len = sizeof(buf);
    out->unique = unique;
    wout->size = count;

    return send_reply(ctx, out, sizeof(buf));
}

static int reply_statfs(struct fuse_dev_ctx *ctx, uint64_t unique, const struct statvfs *st)
{
    char buf[sizeof(struct fuse_out_header) + sizeof(struct fuse_statfs_out)];
    struct fuse_out_header *out = (struct fuse_out_header *)buf;
    struct fuse_statfs_out *sout = (struct fuse_statfs_out *)(buf + sizeof(*out));

    memset(buf, 0, sizeof(buf));
    out->len = sizeof(buf);
    out->unique = unique;
    sout->st.blocks = st->f_blocks;
    sout->st.bfree = st->f_bfree;
    sout->st.bavail = st->f_bavail;
    sout->st.files = st->f_files;
    sout->st.ffree = st->f_ffree;
    sout->st.bsize = st->f_bsize;
    sout->st.namelen = st->f_namemax;
    sout->st.frsize = st->f_frsize;

    return send_reply(ctx, out, sizeof(buf));
}

/* Convert fuse kernel path to string */
static char *get_path(const struct fuse_in_header *inhdr, const char *extra)
{
    size_t len = inhdr->len - sizeof(struct fuse_in_header);
    char *path = malloc(len + 1);
    if (!path) return NULL;
    memcpy(path, extra, len);
    path[len] = '\0';
    return path;
}

/* Helper: use setuid mount helper to perform the FUSE mount */
static int fuse_dev_mount_sudo(struct fuse_dev_ctx *ctx, const char *mountpoint)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("SPERMAFS: socketpair failed");
        return -1;
    }

    char sock_fd_str[32], uid_str[32], gid_str[32], rootmode_str[32];
    snprintf(uid_str, sizeof(uid_str), "%u", getuid());
    snprintf(gid_str, sizeof(gid_str), "%u", getgid());
    snprintf(rootmode_str, sizeof(rootmode_str), "%o", (unsigned int)(S_IFDIR | 0755));

    char self_path[4096];
    ssize_t self_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (self_len <= 0) return -1;
    self_path[self_len] = '\0';

    char *slash = strrchr(self_path, '/');
    if (!slash) return -1;
    *(slash + 1) = '\0';

    char helper_path[4096];
    snprintf(helper_path, sizeof(helper_path), "%sspermfs_mount_helper", self_path);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: close parent's end of socket */
        close(sv[0]);
        char sv_fd_str[16];
        snprintf(sv_fd_str, sizeof(sv_fd_str), "%d", sv[1]);
        execl(helper_path, "spermfs_mount_helper",
              mountpoint, "0",  /* dev_fd placeholder - helper opens own */
              rootmode_str, uid_str, gid_str, sv_fd_str, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        /* Parent: close child's end of socket */
        close(sv[1]);

        /* Receive the /dev/fuse fd from the helper via SCM_RIGHTS */
        struct msghdr msg = {0};
        struct iovec iov;
        char ctrl_buf[CMSG_SPACE(sizeof(int))];
        char dummy;
        iov.iov_base = &dummy;
        iov.iov_len = 1;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl_buf;
        msg.msg_controllen = sizeof(ctrl_buf);

        ssize_t ret = recvmsg(sv[0], &msg, 0);
        if (ret < 0) {
            perror("SPERMAFS: recvmsg failed");
            close(sv[0]);
            return -1;
        }

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
            fprintf(stderr, "SPERMAFS: expected SCM_RIGHTS\n");
            close(sv[0]);
            return -1;
        }
        int received_fd;
        memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
        close(sv[0]);

        /* Wait for child to finish */
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "SPERMAFS: mount helper failed (exit %d)\n",
                    WEXITSTATUS(status));
            close(received_fd);
            return -1;
        }

        ctx->fd = received_fd;
        fprintf(stderr, "SPERMAFS: mounted on %s (fd=%d)\n", mountpoint, ctx->fd);
        return 0;
    } else {
        perror("SPERMAFS: fork failed");
        close(sv[0]);
        close(sv[1]);
    }

    ctx->fd = -1;
    return -1;
}

int fuse_dev_mount(struct fuse_dev_ctx *ctx, const char *mountpoint)
{
    if (!ctx || !mountpoint) return -1;

    strncpy(ctx->mountpoint, mountpoint, sizeof(ctx->mountpoint) - 1);

    ctx->fd = -1;

    /* Try the new mount API directly. If we lack CAP_SYS_ADMIN, retry via setuid helper. */
    int fsfd = syscall(SYS_fsopen, "fuse", FSOPEN_CLOEXEC);
    if (fsfd < 0 && errno == EPERM) {
        return fuse_dev_mount_sudo(ctx, mountpoint);
    }
    if (fsfd < 0) {
        perror("SPERMAFS: fsopen failed, trying mount(2)");
        goto old_api;
    }

    ctx->fd = open(FUSE_DEVICE, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        perror("SPERMAFS: cannot open " FUSE_DEVICE);
        close(fsfd);
        return -1;
    }

    char fd_str[32], rootmode_str[32], uid_str[32], gid_str[32];
    snprintf(fd_str, sizeof(fd_str), "%d", ctx->fd);
    snprintf(rootmode_str, sizeof(rootmode_str), "%o", (unsigned int)(S_IFDIR | 0755));
    snprintf(uid_str, sizeof(uid_str), "%u", getuid());
    snprintf(gid_str, sizeof(gid_str), "%u", getgid());

    if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "source", "spermfs", 0) < 0) {
        perror("SPERMAFS: fsconfig source failed"); goto fail;
    }
    if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "fd", fd_str, 0) < 0) {
        perror("SPERMAFS: fsconfig fd failed"); goto fail;
    }
    if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "rootmode", rootmode_str, 0) < 0) {
        perror("SPERMAFS: fsconfig rootmode failed"); goto fail;
    }
    if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "user_id", uid_str, 0) < 0) {
        perror("SPERMAFS: fsconfig user_id failed"); goto fail;
    }
    if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "group_id", gid_str, 0) < 0) {
        perror("SPERMAFS: fsconfig group_id failed"); goto fail;
    }
    syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_FLAG, "allow_other", NULL, 0);

    if (syscall(SYS_fsconfig, fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0) < 0) {
        perror("SPERMAFS: fsconfig CMD_CREATE failed");
        close(fsfd); close(ctx->fd); ctx->fd = -1;
        return -1;
    }

    int mntfd = syscall(SYS_fsmount, fsfd, FSMOUNT_CLOEXEC, 0);
    if (mntfd < 0) {
        perror("SPERMAFS: fsmount failed");
        close(fsfd); close(ctx->fd); ctx->fd = -1;
        return -1;
    }
    close(fsfd);

    if (syscall(SYS_move_mount, mntfd, "", AT_FDCWD, mountpoint,
                MOVE_MOUNT_F_EMPTY_PATH) < 0) {
        perror("SPERMAFS: move_mount failed");
        close(mntfd); close(ctx->fd); ctx->fd = -1;
        return -1;
    }
    close(mntfd);

    fprintf(stderr, "SPERMAFS: mounted on %s (fd=%d)\n", mountpoint, ctx->fd);
    return 0;

old_api:
    ctx->fd = open(FUSE_DEVICE, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        perror("SPERMAFS: cannot open " FUSE_DEVICE);
        return -1;
    }

    char opts[256];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=%o,user_id=%u,group_id=%u",
             ctx->fd, (unsigned int)(S_IFDIR | 0755), getuid(), getgid());

    if (mount("spermfs", mountpoint, "fuse", 0, opts) < 0) {
        perror("SPERMAFS: mount(2) failed");
        close(ctx->fd); ctx->fd = -1;
        return -1;
    }

    fprintf(stderr, "SPERMAFS: mounted on %s (fd=%d) [mount(2)]\n", mountpoint, ctx->fd);
    return 0;

fail:
    close(fsfd); close(ctx->fd); ctx->fd = -1;
    return -1;
}

struct fuse_readdir_ctx {
    char *buf;
    size_t size;
    size_t off;
};

static int fuse_readdir_filler(void *buf, const char *name,
                                const struct stat *st, off_t off)
{
    struct fuse_readdir_ctx *rd = (struct fuse_readdir_ctx *)buf;
    size_t namelen = strlen(name);
    size_t entry_size = sizeof(struct fuse_dirent) + namelen;
    size_t padded = (entry_size + 7) & ~7;

    if (rd->off + padded > rd->size)
        return 1;

    struct fuse_dirent *fd = (struct fuse_dirent *)(rd->buf + rd->off);
    fd->ino = st ? st->st_ino : 0;
    fd->off = off;
    fd->namelen = namelen;
    fd->type = (st && S_ISDIR(st->st_mode)) ? DT_DIR : DT_REG;
    memcpy(fd->name, name, namelen);
    if (padded > entry_size)
        memset(fd->name + namelen, 0, padded - entry_size);

    rd->off += padded;
    return 0;
}

static char *nodeid_to_path(uint64_t nodeid)
{
    if (nodeid == FUSE_ROOT_ID) {
        char *p = malloc(2);
        if (p) { p[0] = '/'; p[1] = '\0'; }
        return p;
    }
    char *path = malloc(4096);
    if (!path) return NULL;
    if (fuse_bridge_inode_to_path(nodeid, path, 4096) != 0) {
        free(path);
        return NULL;
    }
    return path;
}

int fuse_dev_loop(struct fuse_dev_ctx *ctx, const struct fuse_dev_ops *ops)
{
    if (!ctx || ctx->fd < 0 || !ops) return -1;

    struct fuse_in_header *inhdr;
    char *path = NULL;
    const char *arg = NULL;
    int ret;
    struct stat st;
    struct statvfs stvfs;
    struct fuse_file_info fi;

    ctx->running = 1;

    /* Send INIT response first */
    {
        char init_buf[FUSE_MIN_READ_BUFFER];
        struct fuse_in_header *inh = (struct fuse_in_header *)init_buf;
        ssize_t n = read(ctx->fd, init_buf, sizeof(init_buf));
        if (n < 0) {
            fprintf(stderr, "SPERMAFS: init read failed: errno=%d (%m), fd=%d\n", errno, ctx->fd);
            return -1;
        }
        if (n == 0) { fprintf(stderr, "SPERMAFS: init read returned 0 (EOF)\n"); return -1; }

        if (n >= (ssize_t)sizeof(*inh) && inh->opcode == FUSE_INIT) {
            struct fuse_init_in *init_in = (struct fuse_init_in *)(init_buf + sizeof(*inh));
            (void)init_in;

            struct fuse_init_out init_out;
            memset(&init_out, 0, sizeof(init_out));
            init_out.major = FUSE_MAJOR_VER;
            init_out.minor = FUSE_MINOR_VER;
            init_out.max_readahead = 131072;
            init_out.flags = 0;
            init_out.max_background = 0;
            init_out.congestion_threshold = 0;
            init_out.max_write = FUSE_MAX_WRITE;
            init_out.time_gran = 1;
            init_out.max_pages = 0;

            char reply[sizeof(struct fuse_out_header) + sizeof(init_out)];
            struct fuse_out_header *oh = (struct fuse_out_header *)reply;
            oh->len = sizeof(reply);
            oh->error = 0;
            oh->unique = inh->unique;
            memcpy(reply + sizeof(*oh), &init_out, sizeof(init_out));
            write(ctx->fd, reply, sizeof(reply));

            if (ops->init)
                ops->init(ctx->user_data);
        } else {
            fprintf(stderr, "SPERMAFS: expected INIT, got opcode %d\n", inh->opcode);
            return -1;
        }
    }

    /* Main loop */
    while (ctx->running) {
        struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
        int pret = poll(&pfd, 1, 1000);
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pret == 0) continue;

        ssize_t n = read(ctx->fd, ctx->buf, sizeof(ctx->buf));
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }

        ctx->buf_len = (size_t)n;
        inhdr = (struct fuse_in_header *)ctx->buf;
        arg = ctx->buf + sizeof(struct fuse_in_header);
        size_t arglen = ctx->buf_len - sizeof(struct fuse_in_header);

        {
            static const char *opnames[] = {
                [FUSE_LOOKUP] = "LOOKUP", [FUSE_FORGET] = "FORGET",
                [FUSE_GETATTR] = "GETATTR", [FUSE_SETATTR] = "SETATTR",
                [FUSE_READLINK] = "READLINK", [FUSE_SYMLINK] = "SYMLINK",
                [FUSE_MKNOD] = "MKNOD", [FUSE_MKDIR] = "MKDIR",
                [FUSE_UNLINK] = "UNLINK", [FUSE_RMDIR] = "RMDIR",
                [FUSE_RENAME] = "RENAME", [FUSE_LINK] = "LINK",
                [FUSE_OPEN] = "OPEN", [FUSE_READ] = "READ",
                [FUSE_WRITE] = "WRITE", [FUSE_STATFS] = "STATFS",
                [FUSE_RELEASE] = "RELEASE", [FUSE_FSYNC] = "FSYNC",
                [FUSE_SETXATTR] = "SETXATTR", [FUSE_GETXATTR] = "GETXATTR",
                [FUSE_LISTXATTR] = "LISTXATTR", [FUSE_REMOVEXATTR] = "REMOVEXATTR",
                [FUSE_FLUSH] = "FLUSH", [FUSE_INIT] = "INIT",
                [FUSE_OPENDIR] = "OPENDIR", [FUSE_READDIR] = "READDIR",
                [FUSE_RELEASEDIR] = "RELEASEDIR", [FUSE_FSYNCDIR] = "FSYNCDIR",
                [FUSE_GETLK] = "GETLK", [FUSE_SETLK] = "SETLK",
                [FUSE_SETLKW] = "SETLKW", [FUSE_ACCESS] = "ACCESS",
                [FUSE_CREATE] = "CREATE", [FUSE_INTERRUPT] = "INTERRUPT",
                [FUSE_BMAP] = "BMAP", [FUSE_DESTROY] = "DESTROY",
                [FUSE_IOCTL] = "IOCTL", [FUSE_POLL] = "POLL",
                [FUSE_NOTIFY_REPLY] = "NOTIFY_REPLY",
                [FUSE_BATCH_FORGET] = "BATCH_FORGET",
                [FUSE_FALLOCATE] = "FALLOCATE",
                [FUSE_READDIRPLUS] = "READDIRPLUS",
                [FUSE_RENAME2] = "RENAME2",
                [FUSE_LSEEK] = "LSEEK",
            };
            const char *opname = inhdr->opcode < sizeof(opnames)/sizeof(opnames[0]) ? opnames[inhdr->opcode] : NULL;
            fprintf(stderr, "SPERMAFS_FUSE: op=%s(%d) unique=%llu nodeid=%llu arglen=%u\n",
                    opname ? opname : "?", inhdr->opcode,
                    (unsigned long long)inhdr->unique,
                    (unsigned long long)inhdr->nodeid,
                    inhdr->len - (unsigned int)sizeof(*inhdr));
        }

        switch (inhdr->opcode) {
        case FUSE_LOOKUP: {
            char *name = get_path(inhdr, arg);
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(name);
            free(parent_path);

            memset(&st, 0, sizeof(st));
            ret = ops->getattr ? ops->getattr(path, &st, ctx->user_data) : -ENOSYS;
            if (ret == 0)
                reply_entry(ctx, inhdr->unique, &st, 0, st.st_ino, NULL);
            else
                reply_err(ctx, inhdr->unique, -ret);
            free(path);
            break;
        }

        case FUSE_GETATTR: {
            struct fuse_getattr_in *gi = (struct fuse_getattr_in *)arg;
            (void)gi;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            memset(&st, 0, sizeof(st));
            ret = ops->getattr ? ops->getattr(path, &st, ctx->user_data) : -ENOSYS;
            free(path);

            if (ret == 0)
                reply_attr(ctx, inhdr->unique, &st, 0);
            else
                reply_err(ctx, inhdr->unique, -ret);
            break;
        }

        case FUSE_SETATTR: {
            struct fuse_setattr_in *si = (struct fuse_setattr_in *)arg;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            /* Apply attribute changes */
            if (si->valid & FATTR_MODE) {
                if (ops->chmod) ops->chmod(path, si->mode, ctx->user_data);
            }
            if (si->valid & (FATTR_UID | FATTR_GID)) {
                if (ops->chown) ops->chown(path,
                    (si->valid & FATTR_UID) ? si->uid : (uid_t)-1,
                    (si->valid & FATTR_GID) ? si->gid : (gid_t)-1,
                    ctx->user_data);
            }
            if (si->valid & FATTR_SIZE) {
                if (ops->truncate) ops->truncate(path, si->size, ctx->user_data);
            }
            if (si->valid & FATTR_ATIME || si->valid & FATTR_MTIME) {
                if (ops->utimens) {
                    struct timespec tv[2];
                    if (si->valid & FATTR_ATIME_NOW) {
                        clock_gettime(CLOCK_REALTIME, &tv[0]);
                    } else if (si->valid & FATTR_ATIME) {
                        tv[0].tv_sec = si->atime;
                        tv[0].tv_nsec = si->atimensec;
                    } else {
                        tv[0].tv_sec = 0;
                        tv[0].tv_nsec = UTIME_OMIT;
                    }
                    if (si->valid & FATTR_MTIME_NOW) {
                        clock_gettime(CLOCK_REALTIME, &tv[1]);
                    } else if (si->valid & FATTR_MTIME) {
                        tv[1].tv_sec = si->mtime;
                        tv[1].tv_nsec = si->mtimensec;
                    } else {
                        tv[1].tv_sec = 0;
                        tv[1].tv_nsec = UTIME_OMIT;
                    }
                    ops->utimens(path, tv, ctx->user_data);
                }
            }

            memset(&st, 0, sizeof(st));
            ret = ops->getattr ? ops->getattr(path, &st, ctx->user_data) : -ENOSYS;
            if (ret == 0)
                reply_attr(ctx, inhdr->unique, &st, 0);
            else
                reply_err(ctx, inhdr->unique, -ret);
            free(path);
            break;
        }

        case FUSE_READDIR: {
            struct fuse_read_in *ri = (struct fuse_read_in *)arg;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            char rdbuf[65536];
            struct fuse_readdir_ctx rd;
            rd.buf = rdbuf;
            rd.size = sizeof(rdbuf);
            rd.off = 0;

            memset(&fi, 0, sizeof(fi));

            ret = ops->readdir(path, &rd, fuse_readdir_filler, ri->offset,
                               &fi, ctx->user_data);
            free(path);

            if (ret == 0)
                reply_buf(ctx, inhdr->unique, rdbuf, rd.off);
            else
                reply_err(ctx, inhdr->unique, -ret);
            break;
        }

        case FUSE_OPEN: {
            struct fuse_open_in *oi = (struct fuse_open_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.flags = oi->flags;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->open ? ops->open(path, &fi, ctx->user_data) : 0;
            free(path);

            if (ret == 0)
                reply_open(ctx, inhdr->unique, fi.fh);
            else
                reply_err(ctx, inhdr->unique, -ret);
            break;
        }

        case FUSE_READ: {
            struct fuse_read_in *ri = (struct fuse_read_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.fh = ri->fh;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            char *data = malloc(ri->size + 1);
            if (!data) { free(path); reply_err(ctx, inhdr->unique, ENOMEM); break; }

            int bytes = ops->read ? ops->read(path, data, ri->size, ri->offset, &fi, ctx->user_data) : -ENOSYS;
            free(path);

            if (bytes >= 0)
                reply_buf(ctx, inhdr->unique, data, (size_t)bytes);
            else
                reply_err(ctx, inhdr->unique, -bytes);
            free(data);
            break;
        }

        case FUSE_WRITE: {
            struct fuse_write_in *wi = (struct fuse_write_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.fh = wi->fh;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            const char *write_data = arg + sizeof(struct fuse_write_in);
            size_t write_size = arglen - sizeof(struct fuse_write_in);

            ret = ops->write ? ops->write(path, write_data, write_size, wi->offset, &fi, ctx->user_data) : -ENOSYS;
            free(path);

            if (ret >= 0)
                reply_write(ctx, inhdr->unique, (uint32_t)ret);
            else
                reply_err(ctx, inhdr->unique, -ret);
            break;
        }

        case FUSE_CREATE: {
            struct fuse_create_in *ci = (struct fuse_create_in *)arg;
            char *name = get_path(inhdr, arg + sizeof(*ci));
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            if (ctx->debug)
                fprintf(stderr, "SPERMAFS: CREATE name='%s' parent='%s' path='%s'\n", name, parent_path, path);
            free(name);
            free(parent_path);

            memset(&fi, 0, sizeof(fi));
            fi.flags = ci->flags;

            ret = ops->create ? ops->create(path, ci->mode, &fi, ctx->user_data) : -ENOSYS;
            if (ctx->debug)
                fprintf(stderr, "SPERMAFS: CREATE ret=%d\n", ret);
            if (ret == 0) {
                memset(&st, 0, sizeof(st));
                ops->getattr(path, &st, ctx->user_data);
                char crep[sizeof(struct fuse_out_header) +
                          sizeof(struct fuse_entry_out) +
                          sizeof(struct fuse_open_out)];
                struct fuse_out_header *co = (struct fuse_out_header *)crep;
                struct fuse_entry_out *ce = (struct fuse_entry_out *)(crep + sizeof(*co));
                struct fuse_open_out *cf = (struct fuse_open_out *)(crep + sizeof(*co) + sizeof(*ce));
                memset(crep, 0, sizeof(crep));
                co->len = sizeof(crep);
                co->unique = inhdr->unique;
                ce->nodeid = st.st_ino;
                ce->entry_valid = 0;
                ce->attr_valid = 0;
                fill_attr(&ce->attr, &st);
                cf->fh = fi.fh;
                cf->open_flags = 0;
                if (write(ctx->fd, crep, sizeof(crep)) != (ssize_t)sizeof(crep))
                    perror("SPERMAFS: write to fuse device");
            } else {
                reply_err(ctx, inhdr->unique, -ret);
            }
            free(path);
            break;
        }

        case FUSE_MKNOD: {
            struct fuse_mknod_in *mi = (struct fuse_mknod_in *)arg;
            char *name = get_path(inhdr, arg + sizeof(*mi));
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(name);
            free(parent_path);

            ret = ops->mknod ? ops->mknod(path, mi->mode, mi->rdev, ctx->user_data) : -ENOSYS;
            if (ret == 0) {
                memset(&st, 0, sizeof(st));
                ops->getattr(path, &st, ctx->user_data);
                reply_entry(ctx, inhdr->unique, &st, 0, st.st_ino, NULL);
            } else {
                reply_err(ctx, inhdr->unique, -ret);
            }
            free(path);
            break;
        }

        case FUSE_MKDIR: {
            struct fuse_mkdir_in *mi = (struct fuse_mkdir_in *)arg;
            char *name = get_path(inhdr, arg + sizeof(*mi));
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(name);
            free(parent_path);

            ret = ops->mkdir ? ops->mkdir(path, mi->mode, ctx->user_data) : -ENOSYS;
            if (ret == 0) {
                memset(&st, 0, sizeof(st));
                ops->getattr(path, &st, ctx->user_data);
                reply_entry(ctx, inhdr->unique, &st, 0, st.st_ino, NULL);
            } else {
                reply_err(ctx, inhdr->unique, -ret);
            }
            free(path);
            break;
        }

        case FUSE_SYMLINK: {
            /* FUSE_SYMLINK: no struct, just name\0target\0 */
            char *name = get_path(inhdr, arg);
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *target = name + strlen(name) + 1;
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(parent_path);

            /* Copy target before freeing name */
            char target_buf[1024];
            strncpy(target_buf, target, sizeof(target_buf) - 1);
            target_buf[sizeof(target_buf) - 1] = '\0';
            free(name);

            ret = ops->symlink ? ops->symlink(target_buf, path, ctx->user_data) : -ENOSYS;
            if (ret == 0) {
                memset(&st, 0, sizeof(st));
                ops->getattr(path, &st, ctx->user_data);
                reply_entry(ctx, inhdr->unique, &st, 0, st.st_ino, NULL);
            } else {
                reply_err(ctx, inhdr->unique, -ret);
            }
            free(path);
            break;
        }

        case FUSE_READLINK: {
            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            char linkbuf[1024 + 1];
            ret = ops->readlink ? ops->readlink(path, linkbuf, 1024, ctx->user_data) : -ENOSYS;
            if (ret == 0) {
                size_t link_len = strlen(linkbuf);
                char rep[sizeof(struct fuse_out_header) + link_len + 1];
                struct fuse_out_header *ro = (struct fuse_out_header *)rep;
                ro->len = sizeof(rep);
                ro->unique = inhdr->unique;
                ro->error = 0;
                memcpy(rep + sizeof(*ro), linkbuf, link_len + 1);
                if (write(ctx->fd, rep, sizeof(rep)) != (ssize_t)sizeof(rep))
                    perror("SPERMAFS: write to fuse device");
            } else {
                reply_err(ctx, inhdr->unique, -ret);
            }
            free(path);
            break;
        }

        case FUSE_LINK: {
            struct fuse_link_in *li = (struct fuse_link_in *)arg;
            char *name = get_path(inhdr, arg + sizeof(*li));
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *old_path = nodeid_to_path(li->oldnodeid);
            if (!old_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(old_path); free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(old_path); free(parent_path); free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(name);
            free(parent_path);

            ret = ops->link ? ops->link(old_path, path, ctx->user_data) : -ENOSYS;
            free(old_path);
            if (ret == 0) {
                memset(&st, 0, sizeof(st));
                ops->getattr(path, &st, ctx->user_data);
                reply_entry(ctx, inhdr->unique, &st, 0, st.st_ino, NULL);
            } else {
                reply_err(ctx, inhdr->unique, -ret);
            }
            free(path);
            break;
        }

        case FUSE_UNLINK: {
            char *name = get_path(inhdr, arg);
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(name);
            free(parent_path);

            ret = ops->unlink ? ops->unlink(path, ctx->user_data) : -ENOSYS;
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            free(path);
            break;
        }

        case FUSE_RMDIR: {
            char *name = get_path(inhdr, arg);
            if (!name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');
            path = malloc(plen + need_slash + strlen(name) + 1);
            if (!path) { free(name); free(parent_path); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            sprintf(path, "%s%s%s", parent_path, need_slash ? "/" : "", name);
            free(name);
            free(parent_path);

            ret = ops->rmdir ? ops->rmdir(path, ctx->user_data) : -ENOSYS;
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            free(path);
            break;
        }

        case FUSE_RENAME: {
            struct fuse_rename_in *ri = (struct fuse_rename_in *)arg;
            /* Read source and destination names (relative to nodeid) */
            char *old_name = get_path(inhdr, arg + sizeof(*ri));
            if (!old_name) { reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t old_name_len = strlen(old_name) + 1;
            char *new_name = get_path(inhdr, arg + sizeof(*ri) + old_name_len);
            if (!new_name) { free(old_name); reply_err(ctx, inhdr->unique, ENOMEM); break; }

            char *parent_path = nodeid_to_path(inhdr->nodeid);
            if (!parent_path) { free(old_name); free(new_name); reply_err(ctx, inhdr->unique, ENOMEM); break; }
            size_t plen = strlen(parent_path);
            int need_slash = (plen > 0 && parent_path[plen-1] != '/');

            char *old_path = malloc(plen + need_slash + strlen(old_name) + 1);
            char *new_path = malloc(plen + need_slash + strlen(new_name) + 1);
            if (!old_path || !new_path) {
                free(old_name); free(new_name); free(parent_path);
                free(old_path); free(new_path);
                reply_err(ctx, inhdr->unique, ENOMEM); break;
            }
            sprintf(old_path, "%s%s%s", parent_path, need_slash ? "/" : "", old_name);
            sprintf(new_path, "%s%s%s", parent_path, need_slash ? "/" : "", new_name);
            free(old_name);
            free(new_name);
            free(parent_path);

            ret = ops->rename ? ops->rename(old_path, new_path, 0, ctx->user_data) : -ENOSYS;
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            free(old_path);
            free(new_path);
            break;
        }

        case FUSE_STATFS:
            memset(&stvfs, 0, sizeof(stvfs));
            ret = ops->statfs ? ops->statfs("/", &stvfs, ctx->user_data) : -ENOSYS;
            if (ret == 0)
                reply_statfs(ctx, inhdr->unique, &stvfs);
            else
                reply_err(ctx, inhdr->unique, -ret);
            break;

        case FUSE_RELEASE: {
            struct fuse_release_in *ri = (struct fuse_release_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.fh = ri->fh;
            fi.flags = ri->flags;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->release ? ops->release(path, &fi, ctx->user_data) : 0;
            free(path);
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            break;
        }

        case FUSE_FSYNC: {
            struct fuse_fsync_in *fsi = (struct fuse_fsync_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.fh = fsi->fh;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->fsync ? ops->fsync(path, fsi->fsync_flags & 1, &fi, ctx->user_data) : 0;
            free(path);
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            break;
        }

        case FUSE_FLUSH: {
            struct fuse_flush_in *fli = (struct fuse_flush_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.fh = fli->fh;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->flush ? ops->flush(path, &fi, ctx->user_data) : 0;
            free(path);
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            break;
        }

        case FUSE_FORGET:
            /* Kernel doesn't expect a reply for FORGET */
            if (ctx->debug)
                fprintf(stderr, "SPERMAFS: FORGET nodeid=%llu nlookup=%llu\n",
                        (unsigned long long)inhdr->nodeid,
                        (unsigned long long)((struct fuse_forget_in *)arg)->nlookup);
            continue;

        case FUSE_OPENDIR: {
            struct fuse_open_in *oi = (struct fuse_open_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.flags = oi->flags;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->opendir ? ops->opendir(path, &fi, ctx->user_data) : 0;
            free(path);

            if (ret == 0)
                reply_open(ctx, inhdr->unique, fi.fh);
            else
                reply_err(ctx, inhdr->unique, -ret);
            break;
        }

        case FUSE_RELEASEDIR: {
            struct fuse_release_in *ri = (struct fuse_release_in *)arg;
            memset(&fi, 0, sizeof(fi));
            fi.fh = ri->fh;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->releasedir ? ops->releasedir(path, &fi, ctx->user_data) : 0;
            free(path);
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            break;
        }

        case FUSE_ACCESS: {
            struct fuse_access_in *ai = (struct fuse_access_in *)arg;

            path = nodeid_to_path(inhdr->nodeid);
            if (!path) { reply_err(ctx, inhdr->unique, ENOMEM); break; }

            ret = ops->access ? ops->access(path, ai->mask, ctx->user_data) : 0;
            free(path);
            reply_err(ctx, inhdr->unique, ret == 0 ? 0 : -ret);
            break;
        }

        case FUSE_DESTROY:
            if (ops->destroy) ops->destroy(ctx->user_data);
            reply_err(ctx, inhdr->unique, 0);
            ctx->running = 0;
            break;

        default:
            if (ctx->debug)
                fprintf(stderr, "SPERMAFS: unhandled opcode %d\n", inhdr->opcode);
            reply_err(ctx, inhdr->unique, ENOSYS);
            break;
        }
    }

    return 0;
}

void fuse_dev_unmount(struct fuse_dev_ctx *ctx)
{
    if (!ctx) return;
    ctx->running = 0;
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    umount2(ctx->mountpoint, MNT_DETACH);
    fprintf(stderr, "SPERMAFS: unmounted %s\n", ctx->mountpoint);
}
