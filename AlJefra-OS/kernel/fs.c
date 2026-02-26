/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- In-Kernel BMFS Filesystem Implementation
 *
 * Provides file-level access over the raw sector interface exposed by
 * the storage driver subsystem (driver_get_storage()).
 *
 * BMFS layout on disk:
 *   Byte 1024:  4-byte tag "BMFS"
 *   Byte 4096:  Directory (64 entries x 64 bytes = 4096 bytes)
 *   Block N:    File data at byte offset N * BMFS_BLOCK_SIZE
 *
 * All disk I/O goes through the first available storage driver's
 * read/write ops, which operate in 512-byte sectors.
 */

#include "fs.h"
#include "driver_loader.h"
#include "../hal/hal.h"
#include "../lib/string.h"

/* ---- Internal state ---- */

/* Cached copy of the 4096-byte BMFS directory */
static bmfs_entry_t g_dir[BMFS_MAX_FILES];

/* File descriptor table */
static fs_fd_t g_fds[BMFS_MAX_FILES];

/* LBA offset of the BMFS partition on the storage device */
static uint64_t g_partition_lba;

/* Storage driver ops pointer (cached at init) */
static const driver_ops_t *g_storage;

/* Whether fs_init has been called successfully */
static int g_initialized;

/* Scratch buffer for sector-aligned I/O (one sector).
 * Allocated from DMA memory at init time so it is always available. */
static uint8_t *g_sector_buf;

/* ---- Internal: low-level sector I/O ---- */

/* Read `count` sectors starting at `lba` (relative to BMFS partition) into buf.
 * `buf` must be large enough for count * FS_SECTOR_SIZE bytes. */
static int fs_read_sectors(void *buf, uint64_t lba, uint32_t count)
{
    if (!g_storage || !g_storage->read)
        return -1;
    int64_t rc = g_storage->read(buf, g_partition_lba + lba, count);
    return (rc > 0) ? 0 : -1;
}

/* Write `count` sectors starting at `lba` (relative to BMFS partition) from buf. */
static int fs_write_sectors(const void *buf, uint64_t lba, uint32_t count)
{
    if (!g_storage || !g_storage->write)
        return -1;
    int64_t rc = g_storage->write(buf, g_partition_lba + lba, count);
    return (rc > 0) ? 0 : -1;
}

/* Convert a byte offset within the BMFS partition to a sector number. */
static uint64_t byte_to_sector(uint64_t byte_offset)
{
    return byte_offset / FS_SECTOR_SIZE;
}

/* ---- Internal: directory I/O ---- */

/* Read the BMFS directory from disk into g_dir[]. */
static int dir_read(void)
{
    /* Directory is at byte 4096, which is sector 8 (4096 / 512 = 8).
     * It occupies 4096 bytes = 8 sectors. */
    uint64_t dir_sector = byte_to_sector(BMFS_DIR_OFFSET);
    uint32_t dir_sectors = BMFS_DIR_SIZE / FS_SECTOR_SIZE;

    return fs_read_sectors(g_dir, dir_sector, dir_sectors);
}

/* Write the in-memory directory back to disk. */
static int dir_write(void)
{
    uint64_t dir_sector = byte_to_sector(BMFS_DIR_OFFSET);
    uint32_t dir_sectors = BMFS_DIR_SIZE / FS_SECTOR_SIZE;

    return fs_write_sectors(g_dir, dir_sector, dir_sectors);
}

/* ---- Internal: directory helpers ---- */

/* Compare a filename to a directory entry name.
 * Returns 1 if they match, 0 otherwise. */
static int name_match(const char *name, const char *entry_name)
{
    int i;
    for (i = 0; i < BMFS_MAX_FILENAME; i++) {
        if (name[i] != entry_name[i])
            return 0;
        if (name[i] == '\0')
            return 1;
    }
    /* Both strings filled the entire 32-byte field without NUL */
    return 1;
}

/* Find a directory entry by name.
 * Returns the index (0..63) or -1 if not found. */
static int dir_find(const char *name)
{
    for (int i = 0; i < BMFS_MAX_FILES; i++) {
        uint8_t first = (uint8_t)g_dir[i].filename[0];

        if (first == BMFS_ENTRY_END)
            break;  /* End of directory */

        if (first == BMFS_ENTRY_DELETED)
            continue;  /* Skip deleted entries */

        if (name_match(name, g_dir[i].filename))
            return i;
    }
    return -1;
}

/* Find a free directory slot.
 * Prefers deleted entries (0x01), otherwise uses the end-of-dir slot (0x00).
 * Returns slot index or -1 if directory is full. */
static int dir_find_free(void)
{
    int end_slot = -1;

    for (int i = 0; i < BMFS_MAX_FILES; i++) {
        uint8_t first = (uint8_t)g_dir[i].filename[0];

        if (first == BMFS_ENTRY_DELETED)
            return i;  /* Reuse deleted slot */

        if (first == BMFS_ENTRY_END) {
            end_slot = i;
            break;
        }
    }

    return end_slot;  /* -1 if we scanned all 64 without finding end or deleted */
}

/* Count the number of active (non-deleted, non-end) entries.
 * Also returns the index one past the last used/deleted entry. */
static int dir_count_used(void)
{
    int count = 0;
    for (int i = 0; i < BMFS_MAX_FILES; i++) {
        uint8_t first = (uint8_t)g_dir[i].filename[0];
        if (first == BMFS_ENTRY_END)
            break;
        if (first != BMFS_ENTRY_DELETED)
            count++;
    }
    return count;
}

/* ---- Internal: block allocation ---- */

/* Find `needed` contiguous free blocks on disk.
 * Returns the starting block index, or 0 on failure.
 * Block 0 is reserved (directory/metadata area), so valid blocks start at 1.
 *
 * Strategy: simple first-fit scan.  Collect all allocated regions, sort by
 * starting block, and find the first gap large enough.
 */
static uint64_t find_free_blocks(uint64_t needed)
{
    /* Collect allocated regions */
    typedef struct {
        uint64_t start;
        uint64_t count;
    } region_t;

    region_t regions[BMFS_MAX_FILES];
    int nregions = 0;

    for (int i = 0; i < BMFS_MAX_FILES; i++) {
        uint8_t first = (uint8_t)g_dir[i].filename[0];
        if (first == BMFS_ENTRY_END)
            break;
        if (first == BMFS_ENTRY_DELETED)
            continue;
        regions[nregions].start = g_dir[i].starting_block;
        regions[nregions].count = g_dir[i].reserved_blocks;
        nregions++;
    }

    /* Simple insertion sort by starting block (max 64 entries) */
    for (int i = 1; i < nregions; i++) {
        region_t tmp = regions[i];
        int j = i - 1;
        while (j >= 0 && regions[j].start > tmp.start) {
            regions[j + 1] = regions[j];
            j--;
        }
        regions[j + 1] = tmp;
    }

    /* Scan for gaps.  The first usable block is 1 (block 0 holds metadata). */
    uint64_t candidate = 1;

    for (int i = 0; i < nregions; i++) {
        if (regions[i].start >= candidate + needed) {
            /* Gap before this region is large enough */
            return candidate;
        }
        /* Move candidate past this region */
        uint64_t end = regions[i].start + regions[i].count;
        if (end > candidate)
            candidate = end;
    }

    /* Check space after the last allocated region.
     * We cannot know the total disk size from BMFS metadata alone,
     * so we trust that the caller has reserved a reasonable amount.
     * Just return the candidate if it is non-zero. */
    return candidate;
}

/* ---- Internal: byte-range I/O through sectors ---- */

/* Read arbitrary bytes from disk at a given absolute byte offset within
 * the BMFS partition.  Handles partial sector alignment.
 * `buf` is the destination.  Returns 0 on success, -1 on error. */
static int fs_read_bytes(void *buf, uint64_t byte_offset, uint64_t count)
{
    uint8_t *dst = (uint8_t *)buf;

    while (count > 0) {
        uint64_t sector = byte_to_sector(byte_offset);
        uint32_t offset_in_sector = (uint32_t)(byte_offset % FS_SECTOR_SIZE);
        uint32_t avail = FS_SECTOR_SIZE - offset_in_sector;

        if (offset_in_sector == 0 && count >= FS_SECTOR_SIZE) {
            /* Full aligned sector read -- go directly into dst */
            uint32_t nsectors = (uint32_t)(count / FS_SECTOR_SIZE);
            /* Limit to a reasonable chunk to avoid huge single reads */
            if (nsectors > 256)
                nsectors = 256;

            if (fs_read_sectors(dst, sector, nsectors) < 0)
                return -1;

            uint64_t bytes = (uint64_t)nsectors * FS_SECTOR_SIZE;
            dst += bytes;
            byte_offset += bytes;
            count -= bytes;
        } else {
            /* Partial sector -- read through scratch buffer */
            if (fs_read_sectors(g_sector_buf, sector, 1) < 0)
                return -1;

            uint32_t to_copy = avail;
            if (to_copy > count)
                to_copy = (uint32_t)count;

            memcpy(dst, g_sector_buf + offset_in_sector, to_copy);
            dst += to_copy;
            byte_offset += to_copy;
            count -= to_copy;
        }
    }

    return 0;
}

/* Write arbitrary bytes to disk at a given absolute byte offset within
 * the BMFS partition.  Handles read-modify-write for partial sectors.
 * Returns 0 on success, -1 on error. */
static int fs_write_bytes(const void *buf, uint64_t byte_offset, uint64_t count)
{
    const uint8_t *src = (const uint8_t *)buf;

    while (count > 0) {
        uint64_t sector = byte_to_sector(byte_offset);
        uint32_t offset_in_sector = (uint32_t)(byte_offset % FS_SECTOR_SIZE);
        uint32_t avail = FS_SECTOR_SIZE - offset_in_sector;

        if (offset_in_sector == 0 && count >= FS_SECTOR_SIZE) {
            /* Full aligned sector write -- go directly from src */
            uint32_t nsectors = (uint32_t)(count / FS_SECTOR_SIZE);
            if (nsectors > 256)
                nsectors = 256;

            if (fs_write_sectors(src, sector, nsectors) < 0)
                return -1;

            uint64_t bytes = (uint64_t)nsectors * FS_SECTOR_SIZE;
            src += bytes;
            byte_offset += bytes;
            count -= bytes;
        } else {
            /* Partial sector -- read-modify-write */
            if (fs_read_sectors(g_sector_buf, sector, 1) < 0)
                return -1;

            uint32_t to_copy = avail;
            if (to_copy > count)
                to_copy = (uint32_t)count;

            memcpy(g_sector_buf + offset_in_sector, src, to_copy);

            if (fs_write_sectors(g_sector_buf, sector, 1) < 0)
                return -1;

            src += to_copy;
            byte_offset += to_copy;
            count -= to_copy;
        }
    }

    return 0;
}

/* ---- Public API ---- */

int fs_init(uint64_t partition_lba)
{
    /* Close any previously open fds */
    for (int i = 0; i < BMFS_MAX_FILES; i++)
        g_fds[i].in_use = 0;

    g_initialized = 0;
    g_partition_lba = partition_lba;

    /* Get the active storage driver */
    g_storage = driver_get_storage();
    if (!g_storage) {
        hal_console_puts("[fs] No storage driver available\n");
        return -1;
    }

    if (!g_storage->read) {
        hal_console_puts("[fs] Storage driver has no read op\n");
        return -1;
    }

    /* Allocate a persistent sector-sized DMA buffer for partial I/O */
    if (!g_sector_buf) {
        uint64_t phys;
        g_sector_buf = (uint8_t *)hal_dma_alloc(FS_SECTOR_SIZE, &phys);
        if (!g_sector_buf) {
            hal_console_puts("[fs] Failed to allocate sector buffer\n");
            return -1;
        }
    }

    /* Verify the BMFS tag at byte 1024 */
    char tag[4];
    if (fs_read_bytes(tag, BMFS_TAG_OFFSET, 4) < 0) {
        hal_console_puts("[fs] Failed to read BMFS tag\n");
        return -1;
    }

    if (tag[0] != 'B' || tag[1] != 'M' || tag[2] != 'F' || tag[3] != 'S') {
        hal_console_puts("[fs] Not a BMFS volume (tag mismatch)\n");
        return -1;
    }

    /* Read the directory */
    if (dir_read() < 0) {
        hal_console_puts("[fs] Failed to read directory\n");
        return -1;
    }

    g_initialized = 1;

    int file_count = dir_count_used();
    hal_console_printf("[fs] BMFS mounted: %d file(s)\n", file_count);

    return 0;
}

int fs_init_default(void)
{
    return fs_init(0);
}

int fs_list(fs_list_cb callback, void *ctx)
{
    if (!g_initialized)
        return -1;

    int count = 0;
    for (int i = 0; i < BMFS_MAX_FILES; i++) {
        uint8_t first = (uint8_t)g_dir[i].filename[0];

        if (first == BMFS_ENTRY_END)
            break;

        if (first == BMFS_ENTRY_DELETED)
            continue;

        if (callback)
            callback(g_dir[i].filename, g_dir[i].file_size, ctx);
        count++;
    }

    return count;
}

int fs_open(const char *name)
{
    if (!g_initialized || !name)
        return -1;

    int idx = dir_find(name);
    if (idx < 0)
        return -1;

    /* Check if already open */
    for (int fd = 0; fd < BMFS_MAX_FILES; fd++) {
        if (g_fds[fd].in_use && g_fds[fd].dir_index == idx)
            return fd;  /* Return existing fd */
    }

    /* Find a free fd slot */
    for (int fd = 0; fd < BMFS_MAX_FILES; fd++) {
        if (!g_fds[fd].in_use) {
            g_fds[fd].in_use = 1;
            g_fds[fd].dir_index = idx;
            g_fds[fd].starting_block = g_dir[idx].starting_block;
            g_fds[fd].reserved_blocks = g_dir[idx].reserved_blocks;
            g_fds[fd].file_size = g_dir[idx].file_size;

            /* Copy filename */
            for (int j = 0; j < BMFS_MAX_FILENAME; j++)
                g_fds[fd].filename[j] = g_dir[idx].filename[j];

            return fd;
        }
    }

    return -1;  /* No free fd */
}

int64_t fs_read(int fd, void *buf, uint64_t offset, uint64_t size)
{
    if (!g_initialized || fd < 0 || fd >= BMFS_MAX_FILES)
        return -1;

    fs_fd_t *f = &g_fds[fd];
    if (!f->in_use)
        return -1;

    if (!buf || size == 0)
        return 0;

    /* Clamp to file size */
    if (offset >= f->file_size)
        return 0;

    uint64_t available = f->file_size - offset;
    if (size > available)
        size = available;

    /* Calculate absolute byte offset on disk:
     *   file data starts at starting_block * BMFS_BLOCK_SIZE */
    uint64_t file_disk_start = f->starting_block * BMFS_BLOCK_SIZE;
    uint64_t abs_offset = file_disk_start + offset;

    if (fs_read_bytes(buf, abs_offset, size) < 0)
        return -1;

    return (int64_t)size;
}

int64_t fs_write(int fd, const void *buf, uint64_t offset, uint64_t size)
{
    if (!g_initialized || fd < 0 || fd >= BMFS_MAX_FILES)
        return -1;

    fs_fd_t *f = &g_fds[fd];
    if (!f->in_use)
        return -1;

    if (!g_storage->write) {
        hal_console_puts("[fs] Storage driver has no write op\n");
        return -1;
    }

    if (!buf || size == 0)
        return 0;

    /* Check that the write fits within reserved space */
    uint64_t max_size = f->reserved_blocks * BMFS_BLOCK_SIZE;
    if (offset + size > max_size) {
        hal_console_puts("[fs] Write exceeds reserved space\n");
        return -1;
    }

    /* Calculate absolute byte offset on disk */
    uint64_t file_disk_start = f->starting_block * BMFS_BLOCK_SIZE;
    uint64_t abs_offset = file_disk_start + offset;

    if (fs_write_bytes(buf, abs_offset, size) < 0)
        return -1;

    /* Update file size if we wrote past the current end */
    uint64_t new_end = offset + size;
    if (new_end > f->file_size) {
        f->file_size = new_end;
        g_dir[f->dir_index].file_size = new_end;

        /* Flush updated directory to disk */
        if (dir_write() < 0) {
            hal_console_puts("[fs] Warning: failed to flush directory after write\n");
        }
    }

    return (int64_t)size;
}

uint64_t fs_size(int fd)
{
    if (!g_initialized || fd < 0 || fd >= BMFS_MAX_FILES)
        return 0;

    if (!g_fds[fd].in_use)
        return 0;

    return g_fds[fd].file_size;
}

void fs_close(int fd)
{
    if (fd < 0 || fd >= BMFS_MAX_FILES)
        return;

    g_fds[fd].in_use = 0;
}

int fs_create(const char *name, uint64_t reserved_blocks)
{
    if (!g_initialized || !name)
        return -1;

    if (!g_storage->write) {
        hal_console_puts("[fs] Storage driver has no write op\n");
        return -1;
    }

    /* Check filename length */
    uint32_t len = 0;
    while (name[len] != '\0') {
        len++;
        if (len >= BMFS_MAX_FILENAME) {
            hal_console_puts("[fs] Filename too long\n");
            return -1;
        }
    }

    if (len == 0)
        return -1;

    /* Check that the name doesn't start with a sentinel byte */
    if ((uint8_t)name[0] == BMFS_ENTRY_END || (uint8_t)name[0] == BMFS_ENTRY_DELETED)
        return -1;

    /* Check that a file with this name doesn't already exist */
    if (dir_find(name) >= 0) {
        hal_console_puts("[fs] File already exists\n");
        return -1;
    }

    /* Ensure reserved_blocks >= 1 */
    if (reserved_blocks == 0)
        reserved_blocks = 1;

    /* Find a free directory slot */
    int slot = dir_find_free();
    if (slot < 0) {
        hal_console_puts("[fs] Directory full\n");
        return -1;
    }

    /* Find contiguous free blocks */
    uint64_t start_block = find_free_blocks(reserved_blocks);
    if (start_block == 0) {
        hal_console_puts("[fs] No free blocks\n");
        return -1;
    }

    /* Populate the directory entry */
    memset(&g_dir[slot], 0, sizeof(bmfs_entry_t));

    /* Copy filename with NUL padding */
    for (uint32_t i = 0; i < BMFS_MAX_FILENAME; i++) {
        if (i < len)
            g_dir[slot].filename[i] = name[i];
        else
            g_dir[slot].filename[i] = '\0';
    }

    g_dir[slot].starting_block = start_block;
    g_dir[slot].reserved_blocks = reserved_blocks;
    g_dir[slot].file_size = 0;
    g_dir[slot].unused = 0;

    /* Ensure an end-of-directory marker exists after the new entry.
     * If we overwrote the old end-of-dir slot (0x00), we need to place
     * a new one at slot+1.  If we reused a deleted slot (0x01), the
     * existing end marker is still intact further down. */
    {
        int has_end = 0;
        for (int i = slot + 1; i < BMFS_MAX_FILES; i++) {
            if ((uint8_t)g_dir[i].filename[0] == BMFS_ENTRY_END) {
                has_end = 1;
                break;
            }
        }
        if (!has_end && slot + 1 < BMFS_MAX_FILES) {
            g_dir[slot + 1].filename[0] = BMFS_ENTRY_END;
        }
    }

    /* Flush directory to disk */
    if (dir_write() < 0) {
        hal_console_puts("[fs] Failed to write directory\n");
        return -1;
    }

    hal_console_printf("[fs] Created '%s': block %u, reserved %u blocks\n",
                       name, (uint32_t)start_block, (uint32_t)reserved_blocks);

    return 0;
}

int fs_delete(const char *name)
{
    if (!g_initialized || !name)
        return -1;

    if (!g_storage->write) {
        hal_console_puts("[fs] Storage driver has no write op\n");
        return -1;
    }

    int idx = dir_find(name);
    if (idx < 0) {
        hal_console_puts("[fs] File not found for deletion\n");
        return -1;
    }

    /* Close any open fd pointing to this entry */
    for (int fd = 0; fd < BMFS_MAX_FILES; fd++) {
        if (g_fds[fd].in_use && g_fds[fd].dir_index == idx)
            g_fds[fd].in_use = 0;
    }

    /* Mark entry as deleted */
    g_dir[idx].filename[0] = (char)BMFS_ENTRY_DELETED;

    /* Flush directory to disk */
    if (dir_write() < 0) {
        hal_console_puts("[fs] Failed to write directory\n");
        return -1;
    }

    hal_console_printf("[fs] Deleted file at dir entry %d\n", idx);

    return 0;
}

int fs_sync(void)
{
    if (!g_initialized)
        return -1;

    return dir_write();
}
