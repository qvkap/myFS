# myFS

**S**ecure **P**erformance **E**nhanced **R**eliable **M**odular **A**rchive **F**ile **S**ystem

**Author:** qvkap \<qvkapp@gmail.com\>  
**Version:** 0.1.0 beta  
**License:** MIT

---

## Overview

myFS is a userspace filesystem implemented via FUSE with:

- **B+Tree directory indexing** — fast lookups in large directories (order 128)
- **Copy-on-Write** — per-block clone-on-write for snapshots
- **Journaling** — transaction-based metadata journaling
- **Integrity checking** — CRC64 or SHA-256 checksums on all metadata and data blocks
- **Inline compression** — per-inode ZSTD compression
- **Deduplication** — synchronous content-based dedup via SHA-256
- **Snapshots** — point-in-time snapshots of the B+Tree root
- **Archive mode** — automatic versioned archiving in `/archive`
- **Tiered storage** — multi-device support

---

## Build

### Dependencies

- `gcc` (or `clang`)
- `libfuse3-dev` (>= 3.10)
- `libzstd-dev`
- `libssl-dev` (OpenSSL)
- `libuuid-dev`
- `make`

### Build

```bash
make clean && make -j$(nproc)
```

Two binaries are produced:

| Binary | Purpose |
|--------|---------|
| `spermfs` | Main filesystem daemon + mkfs |
| `spermfs_mount_helper` | setuid mount helper (optional) |

---

## Quick Start

### 1. Create a disk image

```bash
dd if=/dev/zero of=disk.img bs=1M count=64
```

### 2. Format

```bash
./spermfs disk.img --mkfs
```

Optional parameters:

| Flag | Default | Description |
|------|---------|-------------|
| `--blocksize <bytes>` | 65536 | Block size (4096–4194304) |
| `--size <blocks>` | all | Volume size in blocks |
| `--compress <0|1|2>` | 2 (ZSTD) | Compression: 0=none, 1=lz4, 2=zstd |
| `--encrypt <0|1>` | 0 | Encryption: 0=none, 1=AES |

### 3. Mount

```bash
mkdir -p /mnt/spermfs
./spermfs disk.img /mnt/spermfs
```

### 4. Use

```bash
touch /mnt/spermfs/hello.txt
echo "Hello SPERMAFS" > /mnt/spermfs/hello.txt
cat /mnt/spermfs/hello.txt
mkdir /mnt/spermfs/data
ls -la /mnt/spermfs/
```

### 5. Unmount

```bash
fusermount -u /mnt/spermfs
```

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    FUSE Kernel Module                │
├─────────────────────────────────────────────────────┤
│                 /dev/fuse  (custom protocol)         │
├─────────────────────────────────────────────────────┤
│  fuse_bridge.c / fuse_protocol.c  (FUSE op handlers) │
├─────────────────────────────────────────────────────┤
│  spermfs_inode.c   — inode alloc/read/write          │
│  spermfs_btree.c   — B+Tree directory index          │
│  spermfs_extent.c  — extent-based block mapping      │
│  spermfs_cow.c     — copy-on-write                   │
│  spermfs_journal.c — transaction journal             │
│  spermfs_integrity.c — CRC64/SHA-256 checksums       │
│  spermfs_compress.c — ZSTD/LZ4 compression           │
│  spermfs_dedup.c   — content deduplication           │
│  spermfs_snapshot.c — point-in-time snapshots        │
│  spermfs_archive.c — versioned archive store         │
│  spermfs_super.c   — superblock management           │
├─────────────────────────────────────────────────────┤
│  spermfs_read_block / spermfs_write_block  (pread/pwrite) │
└─────────────────────────────────────────────────────┘
```

### On-disk layout

```
Block 0:             Superblock (multiple redundant copies)
Block 1:             B+Tree root node
Blocks 2..N:         Journal (N = total_blocks / 16, max 1024)
Blocks N+1..:        Inode table
Blocks after:        Data blocks (allocated by bump allocator)
```

---

## Operations Reference

### `spermfs` commands

| Command | Description |
|---------|-------------|
| `spermfs <device> <mountpoint>` | Mount filesystem |
| `spermfs <device> --mkfs` | Format device |
| `spermfs --version` | Show version |
| `spermfs --help` | Show usage |

### Mount options

| Option | Description |
|--------|-------------|
| `-f` | Foreground mode |
| `-d` | Debug mode (direct mount(2), no helper) |
| `-s` | Single-threaded |

### File operations supported

- `open`, `read`, `write`, `close`
- `create`, `unlink`
- `mkdir`, `rmdir`
- `readdir`
- `getattr`, `setattr` (truncate, chmod, chown, utimens)
- `statfs`
- `rename`
- `link` (hard links)
- `symlink`, `readlink`

---

## Current Limitations (Beta)

| Area | Limitation |
|------|-----------|
| **Block allocator** | Sequential bump allocator — blocks are never freed or reused |
| **Journal** | Linear log, not circular. No replay on recovery. No overflow guard. |
| **COW** | Old blocks are leaked (no GC / defragmenter) |
| **Deduplication** | In-memory only — lost on remount |
| **Archive** | In-memory only — not persisted. Restore is a no-op. |
| **Snapshots** | Not atomic — no filesystem quiesce |
| **FUSE cache** | attr/entry timeouts = 0 — no kernel caching |
| **Multi-threading** | Not tested with concurrent access |
| **fsck** | No fsck tool available |
| **Mount helper** | Requires `-d` flag or root for setuid helper |

---

## Building for Production

The setuid mount helper avoids needing root for mounting:

```bash
sudo chown root:root spermfs_mount_helper
sudo chmod u+s spermfs_mount_helper
```

Then `./spermfs disk.img /mnt/spermfs` will work without `-d` and without root.

---

## License

MIT License — see LICENSE file.

---

## Contact

qvkap \<qvkapp@gmail.com\>
