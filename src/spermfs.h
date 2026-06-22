#ifndef SPERMAFS_H
#define SPERMAFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <uuid/uuid.h>

#define SPERMAFS_MAGIC           0x535045524D414653ULL
#define SPERMAFS_VERSION_MAJOR   0
#define SPERMAFS_VERSION_MINOR   1
#define SPERMAFS_VERSION_PATCH   0

#define SPERMAFS_MAX_VOLUME_SIZE  (16ULL * 1024 * 1024 * 1024 * 1024 * 1024)
#define SPERMAFS_MAX_FILE_SIZE    (8ULL  * 1024 * 1024 * 1024 * 1024 * 1024)
#define SPERMAFS_MIN_BLOCK_SIZE   4096
#define SPERMAFS_MAX_BLOCK_SIZE   (4ULL * 1024 * 1024)
#define SPERMAFS_DEFAULT_BLOCK_SIZE 65536
#define SPERMAFS_EMBEDDED_MAX     1024
#define SPERMAFS_CRC64_POLY       0xC96C5795D7870F42ULL
#define SPERMAFS_MAX_NAME_LEN     255
#define SPERMAFS_UUID_STR_LEN     37
#define SPERMAFS_MAX_EXTENTS      256
#define SPERMAFS_BTREE_ORDER      128
#define SPERMAFS_DEDUP_HASH_SIZE  32
#define SPERMAFS_NONCE_SIZE       12
#define SPERMAFS_AUTH_TAG_SIZE    16
#define SPERMAFS_KEY_SIZE         32

/* Superblock copy offsets */
#define SPERMAFS_SB_OFFSET_PRIMARY  0ULL
#define SPERMAFS_SB_OFFSET_COPY1    (64ULL  * 1024 * 1024)
#define SPERMAFS_SB_OFFSET_COPY2    (256ULL * 1024 * 1024)
#define SPERMAFS_SB_OFFSET_COPY3    (1ULL   * 1024 * 1024 * 1024)
#define SPERMAFS_NUM_SB_COPIES      4

/* Feature flags */
#define SPERMAFS_FEATURE_JOURNAL     (1ULL << 0)
#define SPERMAFS_FEATURE_COW         (1ULL << 1)
#define SPERMAFS_FEATURE_SNAPSHOTS   (1ULL << 2)
#define SPERMAFS_FEATURE_DEDUP       (1ULL << 3)
#define SPERMAFS_FEATURE_COMPRESSION (1ULL << 4)
#define SPERMAFS_FEATURE_ENCRYPTION  (1ULL << 5)
#define SPERMAFS_FEATURE_INTEGRITY   (1ULL << 6)
#define SPERMAFS_FEATURE_TIERING     (1ULL << 7)
#define SPERMAFS_FEATURE_ARCHIVE     (1ULL << 8)
#define SPERMAFS_FEATURE_ALL         0x1FFULL

/* Inode flags */
#define SPERMAFS_INODE_COMPRESSED    (1 << 0)
#define SPERMAFS_INODE_ENCRYPTED     (1 << 1)
#define SPERMAFS_INODE_DEDUPED       (1 << 2)
#define SPERMAFS_INODE_IMMUTABLE     (1 << 3)
#define SPERMAFS_INODE_APPEND_ONLY   (1 << 4)
#define SPERMAFS_INODE_ARCHIVE       (1 << 5)
#define SPERMAFS_INODE_EMBEDDED      (1 << 6)
#define SPERMAFS_INODE_SNAPSHOT      (1 << 7)

/* Compression algorithms */
#define SPERMAFS_COMPRESS_NONE    0
#define SPERMAFS_COMPRESS_LZ4     1
#define SPERMAFS_COMPRESS_ZSTD    2
#define SPERMAFS_COMPRESS_DEFLATE 3

/* Encryption algorithms */
#define SPERMAFS_CRYPT_NONE            0
#define SPERMAFS_CRYPT_AES256_GCM      1
#define SPERMAFS_CRYPT_XCHACHA20_POLY  2

/* Storage tier types */
#define SPERMAFS_TIER_NVME  0
#define SPERMAFS_TIER_SSD   1
#define SPERMAFS_TIER_HDD   2
#define SPERMAFS_TIER_COUNT 3

/* Journal states */
#define SPERMAFS_JOURNAL_EMPTY    0
#define SPERMAFS_JOURNAL_WRITING  1
#define SPERMAFS_JOURNAL_COMMITTED 2
#define SPERMAFS_JOURNAL_CHECKPOINT 3

/* Snapshot flags */
#define SPERMAFS_SNAP_READ_ONLY  (1 << 0)
#define SPERMAFS_SNAP_WRITABLE   (1 << 1)

/* Error codes */
#define SPERMAFS_OK              0
#define SPERMAFS_ERR_IO          -1
#define SPERMAFS_ERR_CORRUPT     -2
#define SPERMAFS_ERR_NOSPACE     -3
#define SPERMAFS_ERR_EXIST       -4
#define SPERMAFS_ERR_NOENT       -5
#define SPERMAFS_ERR_FULL        -6
#define SPERMAFS_ERR_CHECKSUM    -7
#define SPERMAFS_ERR_CRYPT       -8
#define SPERMAFS_ERR_COMPRESS    -9
#define SPERMAFS_ERR_DEDUP       -10
#define SPERMAFS_ERR_INVAL       -11
#define SPERMAFS_ERR_RO          -12
#define SPERMAFS_ERR_NOMEM       -13

#pragma pack(push, 1)

/* CRC64-protected block header */
typedef struct {
    uint64_t checksum;
    uint64_t block_number;
    uint32_t block_size;
    uint32_t flags;
    uint64_t version;
} __attribute__((aligned(8))) spermfs_block_header_t;

/* Extent - range of contiguous blocks */
typedef struct {
    uint64_t start;
    uint64_t length;
} spermfs_extent_t;

/* Superblock */
typedef struct {
    uint64_t   magic;
    uint32_t   version_major;
    uint32_t   version_minor;
    uint32_t   version_patch;
    uint8_t    uuid[16];
    uint32_t   block_size;
    uint64_t   total_blocks;
    uint64_t   used_blocks;
    uint64_t   features;
    uint64_t   root_tree_root;
    uint64_t   journal_start;
    uint64_t   journal_size;
    uint64_t   journal_head;
    uint64_t   snapshot_root;
    uint64_t   next_inode;
    uint64_t   root_inode;
    uint64_t   archive_inode;
    uint32_t   max_name_len;
    uint8_t    compression_algo;
    uint8_t    encryption_algo;
    uint8_t    reserved[46];
    uint64_t   checksum;
    uint64_t   backup_sb_offsets[3];
} spermfs_superblock_t;

/* Journal entry header */
typedef struct {
    uint64_t sequence;
    uint64_t transaction_id;
    uint64_t inode_number;
    uint64_t offset;
    uint64_t length;
    uint32_t state;
    uint32_t crc32;
    uint64_t timestamp;
    uint8_t  data[];
} spermfs_journal_entry_t;

/* B+Tree node */
typedef struct spermfs_btree_node_s {
    uint64_t   self_block;
    uint64_t   parent_block;
    uint8_t    is_leaf;
    uint16_t   num_keys;
    uint64_t   keys[SPERMAFS_BTREE_ORDER - 1];
    uint64_t   values[SPERMAFS_BTREE_ORDER - 1];
    uint64_t   children[SPERMAFS_BTREE_ORDER];
    uint64_t   checksum;
} __attribute__((aligned(8))) spermfs_btree_node_t;

/* Inode - core metadata structure */
typedef struct {
    uint64_t   inode_number;
    uint32_t   uid;
    uint32_t   gid;
    uint64_t   mode;
    uint64_t   size;
    uint64_t   atime;
    uint64_t   mtime;
    uint64_t   ctime;
    uint64_t   btime;
    uint32_t   link_count;
    uint32_t   flags;
    uint64_t   parent_inode;
    uint8_t    compression_algo;
    uint8_t    encryption_algo;
    uint8_t    dedup_hash[SPERMAFS_DEDUP_HASH_SIZE];
    uint8_t    nonce[SPERMAFS_NONCE_SIZE];
    uint8_t    auth_tag[SPERMAFS_AUTH_TAG_SIZE];
    char       name[SPERMAFS_MAX_NAME_LEN];
    uint64_t   num_extents;
    spermfs_extent_t extents[SPERMAFS_MAX_EXTENTS];
    uint8_t    embedded_data[SPERMAFS_EMBEDDED_MAX];
    uint64_t   checksum;
} __attribute__((aligned(8))) spermfs_inode_t;

/* Snapshot entry */
typedef struct {
    uint64_t   snapshot_id;
    uint64_t   parent_id;
    uint64_t   timestamp;
    uint64_t   root_tree_root;
    uint32_t   flags;
    uint32_t   name_len;
    char       name[64];
    uint64_t   num_changed_blocks;
    uint64_t   changed_blocks[];
} spermfs_snapshot_t;

/* Dedup hash entry */
typedef struct {
    uint8_t    hash[SPERMAFS_DEDUP_HASH_SIZE];
    uint64_t   block_number;
    uint32_t   ref_count;
    uint32_t   block_size;
} spermfs_dedup_entry_t;

/* Tier storage config */
typedef struct {
    int        tier_type;
    char       device_path[256];
    uint64_t   total_blocks;
    uint64_t   used_blocks;
    uint64_t   block_size;
    int        fd;
    uint8_t    reserved[32];
} spermfs_tier_device_t;

/* Archive entry (versioned) */
typedef struct {
    uint64_t   version;
    uint64_t   timestamp;
    uint64_t   inode_number;
    uint64_t   size;
    uint8_t    hash[SPERMAFS_DEDUP_HASH_SIZE];
    uint8_t    reserved[32];
} spermfs_archive_entry_t;

/* Global filesystem context */
typedef struct {
    spermfs_superblock_t    superblock;
    spermfs_tier_device_t   tiers[SPERMAFS_TIER_COUNT];
    int                     num_tiers;
    void                   *btree_cache;
    void                   *inode_cache;
    void                   *dedup_table;
    void                   *snapshot_list;
    uint64_t                current_transaction;
    int                     journal_fd;
    int                     mounted;
    char                    mount_path[1024];
    void                   *user_data;
} spermfs_context_t;

#pragma pack(pop)

/* Function declarations */

/* Superblock */
int  spermfs_super_init(spermfs_context_t *ctx, uint64_t total_blocks, uint32_t block_size);
int  spermfs_super_load(spermfs_context_t *ctx, const char *device);
int  spermfs_super_save(spermfs_context_t *ctx);
int  spermfs_super_find_best(spermfs_context_t *ctx, const char *device);
int  spermfs_super_checksum(spermfs_superblock_t *sb);
void spermfs_super_dump(spermfs_superblock_t *sb);

/* CRC64 */
uint64_t spermfs_crc64(const void *data, size_t len, uint64_t crc);

/* SHA-256 */
int spermfs_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Integrity */
int  spermfs_integrity_check_block(spermfs_context_t *ctx, uint64_t block_num,
                                    void *data, size_t size);
int  spermfs_integrity_write_block(spermfs_context_t *ctx, uint64_t block_num,
                                    void *data, size_t size);
int  spermfs_integrity_check_inode(spermfs_context_t *ctx, spermfs_inode_t *inode);
int  spermfs_integrity_sign_inode(spermfs_context_t *ctx, spermfs_inode_t *inode);
void spermfs_integrity_set_sha256(int enabled);

/* I/O */
int  spermfs_read_block(spermfs_context_t *ctx, uint64_t block_num,
                         void *buf, size_t size, int tier);
int  spermfs_write_block(spermfs_context_t *ctx, uint64_t block_num,
                          void *buf, size_t size, int tier);
int  spermfs_alloc_blocks(spermfs_context_t *ctx, uint64_t count,
                           uint64_t *start_block, int preferred_tier);
void spermfs_free_blocks(spermfs_context_t *ctx, uint64_t start, uint64_t count);

/* B+Tree */
int  spermfs_btree_init(spermfs_context_t *ctx, uint64_t *root_block);
int  spermfs_btree_insert(spermfs_context_t *ctx, uint64_t root_block,
                           uint64_t key, uint64_t value);
int  spermfs_btree_lookup(spermfs_context_t *ctx, uint64_t root_block,
                           uint64_t key, uint64_t *value);
int  spermfs_btree_delete(spermfs_context_t *ctx, uint64_t root_block,
                           uint64_t key);
int  spermfs_btree_update(spermfs_context_t *ctx, uint64_t root_block,
                           uint64_t key, uint64_t value);
int  spermfs_btree_iterate(spermfs_context_t *ctx, uint64_t root_block,
                            int (*cb)(uint64_t key, uint64_t value, void *priv),
                            void *priv);
void spermfs_btree_print(spermfs_context_t *ctx, uint64_t root_block);

/* Inode */
int  spermfs_inode_alloc(spermfs_context_t *ctx, spermfs_inode_t *inode,
                          uint64_t mode);
int  spermfs_inode_read(spermfs_context_t *ctx, uint64_t inode_num,
                         spermfs_inode_t *inode);
int  spermfs_inode_write(spermfs_context_t *ctx, spermfs_inode_t *inode);
int  spermfs_inode_free(spermfs_context_t *ctx, uint64_t inode_num);
int  spermfs_inode_add_extent(spermfs_context_t *ctx, spermfs_inode_t *inode,
                               uint64_t start, uint64_t length);
int  spermfs_inode_remove_extent(spermfs_context_t *ctx, spermfs_inode_t *inode,
                                  uint64_t index);

/* COW */
int  spermfs_cow_write(spermfs_context_t *ctx, spermfs_inode_t *inode,
                        const void *data, uint64_t offset, uint64_t size);
int  spermfs_cow_read(spermfs_context_t *ctx, spermfs_inode_t *inode,
                       void *buf, uint64_t offset, uint64_t size);
int  spermfs_cow_clone_block(spermfs_context_t *ctx, uint64_t old_block,
                              uint64_t *new_block);

/* Journal */
int  spermfs_journal_init(spermfs_context_t *ctx);
int  spermfs_journal_begin(spermfs_context_t *ctx);
int  spermfs_journal_log(spermfs_context_t *ctx, uint64_t inode_num,
                          uint64_t offset, const void *data, uint64_t len);
int  spermfs_journal_commit(spermfs_context_t *ctx);
int  spermfs_journal_rollback(spermfs_context_t *ctx);
int  spermfs_journal_recover(spermfs_context_t *ctx);
void spermfs_journal_destroy(spermfs_context_t *ctx);

/* Snapshot */
int  spermfs_snapshot_create(spermfs_context_t *ctx, const char *name,
                              uint64_t *snap_id);
int  spermfs_snapshot_restore(spermfs_context_t *ctx, uint64_t snap_id);
int  spermfs_snapshot_delete(spermfs_context_t *ctx, uint64_t snap_id);
int  spermfs_snapshot_list(spermfs_context_t *ctx,
                            void (*cb)(uint64_t id, const char *name, uint64_t time));
int  spermfs_snapshot_diff(spermfs_context_t *ctx, uint64_t snap1, uint64_t snap2);

/* Compression */
int  spermfs_compress_init(int algo, int level);
int  spermfs_compress(const void *in, size_t in_len, void *out, size_t *out_len,
                       int algo);
int  spermfs_decompress(const void *in, size_t in_len, void *out, size_t *out_len,
                         int algo);
const char *spermfs_compress_name(int algo);

/* Encryption */
int  spermfs_encrypt_init(int algo, const uint8_t *key, size_t key_len);
int  spermfs_encrypt(spermfs_context_t *ctx, const uint8_t *plain, size_t plain_len,
                      uint8_t *cipher, size_t *cipher_len,
                      uint8_t nonce[12], uint8_t tag[16], int algo,
                      const uint8_t *key);
int  spermfs_decrypt(spermfs_context_t *ctx, const uint8_t *cipher, size_t cipher_len,
                      uint8_t *plain, size_t *plain_len,
                      uint8_t nonce[12], uint8_t tag[16], int algo,
                      const uint8_t *key);
const char *spermfs_crypt_name(int algo);

/* Dedup */
int  spermfs_dedup_init(spermfs_context_t *ctx);
int  spermfs_dedup_find(spermfs_context_t *ctx, const uint8_t *data, size_t len,
                         uint8_t hash[32], uint64_t *block_num);
int  spermfs_dedup_insert(spermfs_context_t *ctx, const uint8_t hash[32],
                           uint64_t block_num, size_t len);
int  spermfs_dedup_ref(spermfs_context_t *ctx, uint64_t block_num);
int  spermfs_dedup_unref(spermfs_context_t *ctx, uint64_t block_num);

/* Tier */
int  spermfs_tier_init(spermfs_context_t *ctx, const char *nvme_dev,
                        const char *ssd_dev, const char *hdd_dev);
int  spermfs_tier_write(spermfs_context_t *ctx, int tier, uint64_t block_num,
                         const void *data, size_t size);
int  spermfs_tier_read(spermfs_context_t *ctx, int tier, uint64_t block_num,
                        void *buf, size_t size);
int  spermfs_tier_migrate(spermfs_context_t *ctx, uint64_t block_num,
                           int from_tier, int to_tier);
int  spermfs_tier_select(spermfs_context_t *ctx, uint64_t inode_num,
                          uint64_t access_count);
void spermfs_tier_track_access(uint64_t inode_num, int tier);

/* Archive */
int  spermfs_archive_init(spermfs_context_t *ctx);
int  spermfs_archive_store(spermfs_context_t *ctx, uint64_t inode_num);
int  spermfs_archive_restore(spermfs_context_t *ctx, uint64_t inode_num,
                              uint64_t version);
int  spermfs_archive_list(spermfs_context_t *ctx, uint64_t inode_num);

/* FUSE bridge - defined in fuse_bridge.c */
int  fuse_bridge_main(spermfs_context_t *ctx, int argc, char **argv);

/* Utility */
uint64_t spermfs_time_ns(void);
void     spermfs_uuid_generate(uint8_t uuid[16]);
int      spermfs_crc32(const void *data, size_t len);

#endif /* SPERMAFS_H */
