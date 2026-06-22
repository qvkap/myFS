#include "spermfs.h"
#include "fuse_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>

extern spermfs_context_t *g_ctx;

static void print_banner(void)
{
    printf("SPERMAFS v%d.%d.%d beta - Secure Performance Enhanced Reliable Modular Archive File System\n",
           SPERMAFS_VERSION_MAJOR, SPERMAFS_VERSION_MINOR, SPERMAFS_VERSION_PATCH);
    printf("Copyright (C) 2026 qvkap <qvkapp@gmail.com>\n\n");
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <device> <mountpoint> [options]\n\n", prog);
    printf("Options:\n");
    printf("  -f            Foreground mode\n");
    printf("  -d            Debug mode\n");
    printf("  -s            Single-threaded\n");
    printf("  -o opt        FUSE mount options\n");
    printf("  --mkfs        Format the device as SPERMAFS\n");
    printf("  --blocksize N Block size in bytes (4096, 65536, 1048576, 4194304)\n");
    printf("  --size N      Volume size in blocks\n");
    printf("  --uuid UUID   Set volume UUID\n");
    printf("  --compress N  Compression algorithm (0=none, 1=LZ4, 2=ZSTD, 3=DEFLATE)\n");
    printf("  --encrypt N   Encryption algorithm (0=none, 1=AES-256-GCM, 2=XChaCha20-Poly1305)\n");
    printf("  --no-journal  Disable journaling\n");
    printf("  --no-cow      Disable Copy-on-Write\n");
    printf("  --no-dedup    Disable deduplication\n");
    printf("  --no-compress Disable compression\n");
    printf("  --no-encrypt  Disable encryption\n");
    printf("  --no-integrity Disable integrity checks\n");
    printf("  --version     Show version\n");
    printf("  --help        Show this help\n");
    printf("\n");
    printf("Snapshot commands (after mounting):\n");
    printf("  spermfsctl --snap-create <name>\n");
    printf("  spermfsctl --snap-list\n");
    printf("  spermfsctl --snap-restore <id>\n");
    printf("  spermfsctl --snap-delete <id>\n");
    printf("\n");
    printf("Archive commands:\n");
    printf("  spermfsctl --archive-store <path>\n");
    printf("  spermfsctl --archive-list <path>\n");
    printf("  spermfsctl --archive-restore <path> <version>\n");
}

int main(int argc, char **argv)
{
    print_banner();

    char *device = NULL;
    char *mountpoint = NULL;
    int foreground = 0;
    int singlethread = 0;
    int debug = 0;
    int do_mkfs = 0;
    uint32_t block_size = SPERMAFS_DEFAULT_BLOCK_SIZE;
    uint64_t volume_blocks = 0;
    int compress = SPERMAFS_COMPRESS_ZSTD;
    int encrypt = SPERMAFS_CRYPT_NONE;

    /* Parse arguments */
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mkfs") == 0) {
            do_mkfs = 1;
        } else if (strcmp(argv[i], "--blocksize") == 0 && i + 1 < argc) {
            block_size = (uint32_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            volume_blocks = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--compress") == 0 && i + 1 < argc) {
            compress = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--encrypt") == 0 && i + 1 < argc) {
            encrypt = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-f") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            singlethread = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (device == NULL) {
            device = argv[i];
        } else if (mountpoint == NULL) {
            mountpoint = argv[i];
        }
    }

    if (!device) {
        fprintf(stderr, "Error: no device specified\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Allocate context */
    spermfs_context_t *ctx = calloc(1, sizeof(spermfs_context_t));
    if (!ctx) {
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

    if (do_mkfs) {
        /* Format the device */
        if (volume_blocks == 0) {
            /* Default: 1M blocks */
            volume_blocks = 1024 * 1024;
        }

        if (block_size < SPERMAFS_MIN_BLOCK_SIZE ||
            block_size > SPERMAFS_MAX_BLOCK_SIZE) {
            block_size = SPERMAFS_DEFAULT_BLOCK_SIZE;
        }

        /* Open device (create if size given) */
        ctx->tiers[0].fd = open(device, O_RDWR | O_CREAT, 0644);
        if (ctx->tiers[0].fd < 0) {
            fprintf(stderr, "Error: cannot open device %s: %s\n",
                    device, strerror(errno));
            free(ctx);
            return 1;
        }

        /* Ensure the file is large enough for mkfs */
        off_t file_size = lseek(ctx->tiers[0].fd, 0, SEEK_END);
        if (file_size < (off_t)(volume_blocks * block_size)) {
            /* Fallocate or truncate to desired size */
            if (ftruncate(ctx->tiers[0].fd, (off_t)(volume_blocks * block_size)) < 0) {
                fprintf(stderr, "Error: cannot resize device %s: %s\n",
                        device, strerror(errno));
                close(ctx->tiers[0].fd);
                free(ctx);
                return 1;
            }
        }
        ctx->num_tiers = 1;
        strncpy(ctx->tiers[0].device_path, device,
                sizeof(ctx->tiers[0].device_path) - 1);
        ctx->tiers[0].block_size = block_size;

        /* Initialize superblock */
        int ret = spermfs_super_init(ctx, volume_blocks, block_size);
        if (ret != SPERMAFS_OK) {
            fprintf(stderr, "Error: superblock init failed: %d\n", ret);
            close(ctx->tiers[0].fd);
            free(ctx);
            return 1;
        }

        ctx->superblock.compression_algo = (uint8_t)compress;
        ctx->superblock.encryption_algo = (uint8_t)encrypt;

        /* Initialize B+Tree root */
        ret = spermfs_btree_init(ctx, &ctx->superblock.root_tree_root);
        if (ret != SPERMAFS_OK) {
            fprintf(stderr, "Error: B+Tree init failed: %d\n", ret);
            close(ctx->tiers[0].fd);
            free(ctx);
            return 1;
        }

        /* Init journal BEFORE creating inodes so journal_start/journal_size are set */
        {
            int jret = spermfs_journal_init(ctx);
            if (jret != SPERMAFS_OK) {
                fprintf(stderr, "SPERMAFS: warning: journal init failed (%d), continuing\n", jret);
            }
        }

        /* Create root inode */
        spermfs_inode_t root_inode;
        ret = spermfs_inode_alloc(ctx, &root_inode, S_IFDIR | 0755);
        if (ret != SPERMAFS_OK) {
            fprintf(stderr, "Error: root inode alloc failed: %d\n", ret);
            close(ctx->tiers[0].fd);
            free(ctx);
            return 1;
        }
        ctx->superblock.root_inode = root_inode.inode_number;

        ret = spermfs_inode_write(ctx, &root_inode);
        if (ret != SPERMAFS_OK) {
            fprintf(stderr, "Error: root inode write failed: %d\n", ret);
            close(ctx->tiers[0].fd);
            free(ctx);
            return 1;
        }

        /* Init dedup */
        if (ctx->superblock.features & SPERMAFS_FEATURE_DEDUP) {
            spermfs_dedup_init(ctx);
        }

        /* Init archive */
        spermfs_archive_init(ctx);

        /* Save superblock */
        ret = spermfs_super_save(ctx);
        if (ret == SPERMAFS_OK)
            printf("SPERMAFS: formatted %s (%llu blocks, %u bytes/block)\n",
                   device, (unsigned long long)volume_blocks, block_size);

        close(ctx->tiers[0].fd);
        spermfs_super_dump(&ctx->superblock);
        free(ctx);
        return (ret == SPERMAFS_OK) ? 0 : 1;
    }

    if (!mountpoint) {
        fprintf(stderr, "Error: no mount point specified\n");
        print_usage(argv[0]);
        free(ctx);
        return 1;
    }

    /* Mount existing filesystem */
    int ret = spermfs_super_load(ctx, device);
    if (ret != SPERMAFS_OK) {
        fprintf(stderr, "Error: cannot load SPERMAFS from %s\n", device);
        free(ctx);
        return 1;
    }

    spermfs_super_dump(&ctx->superblock);

    /* Init and recover journal */
    int journal_ret = spermfs_journal_init(ctx);
    if (journal_ret != SPERMAFS_OK) {
        fprintf(stderr, "SPERMAFS: warning: journal init failed (%d), continuing without journal\n",
                journal_ret);
    } else {
        spermfs_journal_recover(ctx);
    }
    if (ctx->superblock.features & SPERMAFS_FEATURE_DEDUP)
        spermfs_dedup_init(ctx);

    if (ctx->superblock.features & SPERMAFS_FEATURE_ARCHIVE)
        spermfs_archive_init(ctx);

    /* Store context globally for FUSE callbacks */
    g_ctx = ctx;
    ctx->mounted = 1;

    /* Run FUSE main loop using direct protocol implementation */
    printf("SPERMAFS: mounting on %s...\n", mountpoint);
    ret = fuse_bridge_main(ctx, argc, argv);

    printf("SPERMAFS: unmounted\n");
    ctx->mounted = 0;

    /* Save superblock on unmount */
    spermfs_super_save(ctx);

    /* Cleanup */
    if (ctx->tiers[0].fd >= 0)
        close(ctx->tiers[0].fd);

    free(ctx);
    return ret;
}
