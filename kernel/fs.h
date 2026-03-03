/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- In-Kernel BMFS Filesystem API
 *
 * Provides file-level access on top of raw sector I/O from the storage
 * driver subsystem.  BMFS is a simple flat filesystem:
 *
 *   - Disk info tag "BMFS" at byte offset 1024
 *   - Directory at byte offset 4096, 4096 bytes (64 entries x 64 bytes)
 *   - Block size = 2 MiB (contiguous allocation, no fragmentation)
 *   - Maximum 64 files
 *
 * Directory entry layout (64 bytes):
 *   [0..31]   FileName   (NUL-padded; 0x00 = end-of-dir, 0x01 = deleted)
 *   [32..39]  StartingBlock  (uint64_t, block index; data at block*2MiB)
 *   [40..47]  ReservedBlocks (uint64_t, number of 2 MiB blocks reserved)
 *   [48..55]  FileSize       (uint64_t, actual file size in bytes)
 *   [56..63]  Unused         (uint64_t, reserved for future use)
 */

#ifndef ALJEFRA_KERNEL_FS_H
#define ALJEFRA_KERNEL_FS_H

#include <stdint.h>

/* ---- Constants ---- */

#define BMFS_BLOCK_SIZE        (2 * 1024 * 1024)  /* 2 MiB */
#define BMFS_DIR_OFFSET        4096               /* Byte offset of directory on disk */
#define BMFS_DIR_SIZE          4096               /* Directory region size in bytes */
#define BMFS_TAG_OFFSET        1024               /* Byte offset of "BMFS" tag */
#define BMFS_MAX_FILES         64                 /* Maximum directory entries */
#define BMFS_MAX_FILENAME      32                 /* Maximum filename length (including NUL) */
#define BMFS_ENTRY_SIZE        64                 /* Size of one directory entry */

/* Sentinel values in FileName[0] */
#define BMFS_ENTRY_END         0x00               /* End of directory marker */
#define BMFS_ENTRY_DELETED     0x01               /* Deleted entry */

/* Sector size assumed for storage driver I/O */
#define FS_SECTOR_SIZE         512

/* ---- On-disk structures ---- */

typedef struct {
    char     filename[BMFS_MAX_FILENAME];   /* NUL-padded file name */
    uint64_t starting_block;                /* First block index */
    uint64_t reserved_blocks;               /* Number of 2 MiB blocks reserved */
    uint64_t file_size;                     /* Actual data size in bytes */
    uint64_t unused;                        /* Reserved */
} __attribute__((packed)) bmfs_entry_t;

/* ---- File descriptor state ---- */

typedef struct {
    int      in_use;                        /* Non-zero if this fd is open */
    int      dir_index;                     /* Index into the on-disk directory */
    uint64_t starting_block;                /* Cached from directory entry */
    uint64_t reserved_blocks;
    uint64_t file_size;
    char     filename[BMFS_MAX_FILENAME];
} fs_fd_t;

/* ---- Callback for fs_list ---- */
typedef void (*fs_list_cb)(const char *name, uint64_t size, void *ctx);

/* ---- Public API ---- */

/* Initialize filesystem: reads the BMFS directory from the boot storage device.
 * Must be called after storage drivers are loaded.
 * `partition_lba` is the LBA offset of the BMFS partition on the storage device
 * (0 if the entire disk is BMFS, or the starting LBA for a hybrid image).
 * Returns 0 on success, -1 on error. */
int fs_init(uint64_t partition_lba);

/* Convenience wrapper: initializes with partition_lba = 0 */
int fs_init_default(void);

/* List all files.  Calls `callback` once per valid file entry.
 * Returns number of files found, or -1 on error. */
int fs_list(fs_list_cb callback, void *ctx);

/* Open a file by name.
 * Returns a file descriptor (0..63) on success, or -1 if not found. */
int fs_open(const char *name);

/* Read `size` bytes from an open file starting at byte `offset` into `buf`.
 * Returns number of bytes actually read, or -1 on error.
 * Reading past end-of-file returns fewer bytes (down to 0). */
int64_t fs_read(int fd, void *buf, uint64_t offset, uint64_t size);

/* Write `size` bytes to an open file starting at byte `offset` from `buf`.
 * The file must have enough reserved space.  Updates file_size if writing
 * past the current end.
 * Returns number of bytes written, or -1 on error. */
int64_t fs_write(int fd, const void *buf, uint64_t offset, uint64_t size);

/* Get the current file size for an open file descriptor.
 * Returns the size, or 0 if the fd is invalid. */
uint64_t fs_size(int fd);

/* Close an open file descriptor. */
void fs_close(int fd);

/* Create a new file with the given number of reserved 2 MiB blocks.
 * The file is created with file_size = 0.
 * Returns 0 on success, -1 on error (no space, name too long, etc). */
int fs_create(const char *name, uint64_t reserved_blocks);

/* Delete a file by name.  Marks the directory entry as deleted.
 * Returns 0 on success, -1 if not found. */
int fs_delete(const char *name);

/* Flush the in-memory directory back to disk.
 * Called automatically by fs_create, fs_delete, and fs_write (on size change).
 * Returns 0 on success, -1 on error. */
int fs_sync(void);

#endif /* ALJEFRA_KERNEL_FS_H */
