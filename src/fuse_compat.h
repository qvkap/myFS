#ifndef FUSE_COMPAT_H
#define FUSE_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/* FUSE version */
#define FUSE_USE_VERSION 31

/* Major/minor for device creation */
#define FUSE_MAJOR(d) (((d) >> 8) & 0xff)
#define FUSE_MINOR(d) ((d) & 0xff)

struct fuse_ctx {
    uid_t uid;
    gid_t gid;
    pid_t pid;
    mode_t umask;
};

struct fuse_file_info {
    int flags;
    unsigned long fh_old;
    int writepage;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    unsigned int flush : 1;
    unsigned int nonseekable : 1;
    unsigned int flock_release : 1;
    unsigned int cache_readdir : 1;
    unsigned int noflush : 1;
    unsigned int no_seek_on_write : 1;
    unsigned int spatial_cache : 1;
    unsigned int nullpath_ok : 1;
    unsigned int parallel_direct_writes : 1;
    unsigned int positive_timeout : 1;
    unsigned int negative_timeout : 1;
    unsigned int no_reader : 1;
    unsigned int no_writer : 1;
    unsigned int reserved : 16;
    uint64_t fh;
    uint64_t lock_owner;
    uint64_t poll_events;
    uint32_t write_flags;
    uint32_t padding;
};

struct fuse_buf {
    size_t size;
    size_t flags;
    void *mem;
    int fd;
    off_t pos;
};

struct fuse_bufvec {
    size_t count;
    size_t idx;
    size_t off;
    struct fuse_buf buf[1];
};

enum fuse_buf_copy_flags {
    FUSE_BUF_NO_SPLICE = 1,
    FUSE_BUF_FORCE_SPLICE = 2,
    FUSE_BUF_SPLICE_MOVE = 4,
    FUSE_BUF_SPLICE_NONBLOCK = 8,
};

struct fuse_conn_info;
struct fuse_config;
struct fuse_pollhandle;
struct fuse_loop_config;
struct fuse_opt;

struct fuse_conn_info {
    unsigned proto_major;
    unsigned proto_minor;
    unsigned max_write;
    unsigned max_read;
    unsigned max_readahead;
    unsigned capable;
    unsigned want;
    unsigned max_background;
    unsigned congestion_threshold;
    unsigned time_gran;
    unsigned reserved[32];
};

struct fuse_config {
    int set_uid;
    int set_gid;
    int set_mode;
    uid_t uid;
    gid_t gid;
    mode_t umask;
    mode_t mode;
    int entry_timeout;
    int negative_timeout;
    int attr_timeout;
    int intr;
    int intr_signal;
    int remember;
    int hard_remove;
    int use_ino;
    int readdir_ino;
    int set_mode_in_kernel;
    int source;
    int nullpath_ok;
    int show_version;
    int show_help;
    int no_splice_read;
    int no_splice_write;
    int no_splice_move;
    int splice_read;
    int splice_write;
    int splice_move;
    int auto_cache;
    int auto_inval_data;
    int writeback_cache;
    int no_rofd_flush;
    int no_readdirplus;
    int async_dio;
    int nopage;
    int async_read;
    int atomic_o_trunc;
    int show_type;
    int no_flush;
    int opr;
    unsigned int max_read;
    unsigned int max_write;
    unsigned int max_readahead;
    char *subtype;
    int posix_acl;
    int no_mgr;
    int no_open;
    int no_opendir;
    int no_open_support;
    int no_access;
    int no_getattr;
    int no_readdir;
    int no_readdirplus_auto;
    unsigned int max_background;
    unsigned int congestion_threshold;
    unsigned int time_gran;
    int nopage_ok;
    unsigned int max_pages;
    unsigned int max_pages_per_read;
    unsigned int max_pages_per_write;
    unsigned int max_pages_limit;
};

struct fuse_pollhandle;
struct fuse_loop_config {
    int clone_fd;
    unsigned int max_idle_threads;
};

struct fuse_operations {
    int (*getattr)(const char *, struct stat *, struct fuse_file_info *);
    int (*readlink)(const char *, char *, size_t);
    int (*mknod)(const char *, mode_t, dev_t);
    int (*mkdir)(const char *, mode_t);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*symlink)(const char *, const char *);
    int (*rename)(const char *, const char *, unsigned int);
    int (*link)(const char *, const char *);
    int (*chmod)(const char *, mode_t, struct fuse_file_info *);
    int (*chown)(const char *, uid_t, gid_t, struct fuse_file_info *);
    int (*truncate)(const char *, off_t, struct fuse_file_info *);
    int (*open)(const char *, struct fuse_file_info *);
    int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
    int (*write)(const char *, const char *, size_t, off_t, struct fuse_file_info *);
    int (*statfs)(const char *, struct statvfs *);
    int (*flush)(const char *, struct fuse_file_info *);
    int (*release)(const char *, struct fuse_file_info *);
    int (*fsync)(const char *, int, struct fuse_file_info *);
    int (*setxattr)(const char *, const char *, const char *, size_t, int);
    int (*getxattr)(const char *, const char *, char *, size_t);
    int (*listxattr)(const char *, char *, size_t);
    int (*removexattr)(const char *, const char *);
    int (*opendir)(const char *, struct fuse_file_info *);
    int (*readdir)(const char *, void *, int (*filler)(void *, const char *, const struct stat *, off_t, unsigned int), off_t, struct fuse_file_info *, unsigned int);
    int (*releasedir)(const char *, struct fuse_file_info *);
    int (*fsyncdir)(const char *, int, struct fuse_file_info *);
    void *(*init)(struct fuse_conn_info *conn, struct fuse_config *cfg);
    void (*destroy)(void *);
    int (*access)(const char *, int);
    int (*create)(const char *, mode_t, struct fuse_file_info *);
    int (*lock)(const char *, struct fuse_file_info *, int cmd, struct flock *);
    int (*utimens)(const char *, const struct timespec tv[2], struct fuse_file_info *);
    int (*bmap)(const char *, size_t blocksize, uint64_t *idx);
    int (*ioctl)(const char *, unsigned int cmd, void *arg, struct fuse_file_info *, unsigned int flags, void *data);
    int (*poll)(const char *, struct fuse_file_info *, struct fuse_pollhandle *ph, unsigned *reventsp);
    int (*write_buf)(const char *, struct fuse_bufvec *buf, off_t off, struct fuse_file_info *);
    int (*read_buf)(const char *, struct fuse_bufvec **bufp, size_t size, off_t off, struct fuse_file_info *);
    int (*flock)(const char *, struct fuse_file_info *, int op);
    int (*fallocate)(const char *, int, off_t, off_t, struct fuse_file_info *);
    int (*copy_file_range)(const char *, struct fuse_file_info *, off_t, const char *, struct fuse_file_info *, off_t, size_t, int flags);
    ssize_t (*lseek)(const char *, off_t, int, struct fuse_file_info *);
    void *padding[3];
};

struct fuse_cmdline_opts {
    int singlethread;
    int foreground;
    int debug;
    int nodefault_subtype;
    char *mountpoint;
    int show_version;
    int show_help;
    int clone_fd;
    unsigned int max_idle_threads;
};

struct fuse_pollhandle;

struct fuse_args {
    int argc;
    char **argv;
    int allocated;
};

struct fuse_chan;

struct fuse_session;

struct fuse {
    struct fuse_session *se;
    struct fuse_chan *ch;
    int fd;
    struct fuse_operations op;
    size_t op_size;
    void *user_data;
    uid_t uid;
    gid_t gid;
    pid_t pid;
    int intr_interval;
    unsigned int max_write;
    unsigned int max_read;
    unsigned int max_readahead;
    int async_read;
    int atomic_o_trunc;
    int writeback_cache;
    int no_rofd_flush;
    int no_readdirplus;
    int no_open;
    int no_opendir;
    int no_open_support;
    int no_access;
    int no_getattr;
    int no_readdir;
    int no_readdirplus_auto;
    int no_splice_read;
    int no_splice_write;
    int no_splice_move;
    int splice_read;
    int splice_write;
    int splice_move;
    int auto_cache;
    int auto_inval_data;
    char *subtype;
};

/* Forward declarations */
struct fuse_opt;
#ifndef _SYS_FILE_H
struct flock;
#endif

/* Function declarations (from libfuse3.so) */
extern int fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
                           size_t op_size, void *user_data);
extern struct fuse *fuse_new_31(struct fuse_args *args,
                                 const struct fuse_operations *op,
                                 size_t op_size, void *user_data);
extern void fuse_destroy(struct fuse *f);
extern int fuse_loop(struct fuse *f);
extern int fuse_loop_mt_312(struct fuse *f, struct fuse_loop_config *config);
extern void fuse_exit(struct fuse *f);
extern struct fuse_session *fuse_get_session(struct fuse *f);
extern int fuse_session_loop(struct fuse_session *se);
extern int fuse_session_loop_mt_312(struct fuse_session *se, struct fuse_loop_config *config);
extern void fuse_session_destroy(struct fuse_session *se);
extern int fuse_session_exited(struct fuse_session *se);
extern void fuse_session_exit(struct fuse_session *se);
extern int fuse_session_mount(struct fuse_session *se, const char *mountpoint);
extern struct fuse_session *fuse_session_new(struct fuse_args *args,
                                              const struct fuse_operations *op,
                                              size_t op_size, void *user_data);
extern void fuse_session_unmount(struct fuse_session *se);
extern struct fuse_chan *fuse_mount(const char *mountpoint, struct fuse_args *args);
extern void fuse_unmount(const char *mountpoint, struct fuse_chan *ch);
extern int fuse_parse_cmdline_312(struct fuse_args *args,
                                   struct fuse_cmdline_opts *opts);
extern void fuse_cmdline_help(void);
extern void fuse_opt_free_args(struct fuse_args *args);
extern int fuse_opt_parse(struct fuse_args *args, void *data,
                          const struct fuse_opt *opt, void (*proc)(void *data,
                          const char *arg, int key, struct fuse_args *outargs));
extern int fuse_version(void);
extern const char *fuse_pkgversion(void);
extern void fuse_add_direntry(void *buf, size_t bufsize, const char *name,
                               const struct stat *st, off_t off);
extern int fuse_daemonize(int foreground);
extern int fuse_set_signal_handlers(struct fuse_session *se);
extern void fuse_remove_signal_handlers(struct fuse_session *se);
extern struct fuse_ctx *fuse_get_context(void);

static inline struct fuse_loop_config *fuse_loop_cfg_create(void)
{
    struct fuse_loop_config *cfg = calloc(1, sizeof(struct fuse_loop_config));
    if (cfg) {
        cfg->clone_fd = 0;
        cfg->max_idle_threads = 10;
    }
    return cfg;
}

static inline void fuse_loop_cfg_destroy(struct fuse_loop_config *cfg)
{
    free(cfg);
}

static inline void fuse_loop_cfg_set_clone_fd(struct fuse_loop_config *cfg, int v)
{
    cfg->clone_fd = v;
}

static inline void fuse_loop_cfg_set_max_threads(struct fuse_loop_config *cfg, unsigned int v)
{
    (void)cfg; (void)v;
}

#endif /* FUSE_COMPAT_H */
