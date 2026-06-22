#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "Usage: %s <mountpoint> <dev_fd> <rootmode> <uid> <gid> <sock_fd>\n", argv[0]);
        return 1;
    }

    const char *mountpoint = argv[1];
    const char *rootmode = argv[3];
    const char *uid_str = argv[4];
    const char *gid_str = argv[5];
    int sock_fd = atoi(argv[6]);

    if (geteuid() != 0) {
        fprintf(stderr, "mount helper must run as root\n");
        return 1;
    }

    /* Open /dev/fuse ourselves as root */
    int dev_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (dev_fd < 0) {
        perror("SPERMAFS-helper: open /dev/fuse");
        return 1;
    }

    /* Use the OLD mount API - simple and works */
    char opts[256];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=%s,user_id=%s,group_id=%s",
             dev_fd, rootmode, uid_str, gid_str);

    if (mount("spermfs", mountpoint, "fuse", 0, opts) < 0) {
        perror("SPERMAFS-helper: mount");
        close(dev_fd);
        return 1;
    }

    /* Send dev_fd to parent via SCM_RIGHTS */
    struct msghdr msg = {0};
    struct iovec iov;
    char ctrl_buf[CMSG_SPACE(sizeof(int))];
    char dummy = 0;

    iov.iov_base = &dummy;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &dev_fd, sizeof(int));

    if (sendmsg(sock_fd, &msg, 0) < 0) {
        perror("SPERMAFS-helper: sendmsg");
        return 1;
    }

    return 0;
}
