# AlJefra OS -- BMFS Filesystem

## Overview

AlJefra OS uses the BareMetal File System (BMFS), a simple flat filesystem designed for bare-metal operating systems. BMFS prioritizes simplicity and performance over feature richness -- there are no directories, no permissions, no journaling, and no fragmentation. Files are stored in contiguous 2MB blocks, making read and write operations straightforward memory-mapped operations.

This design aligns with the AlJefra exokernel philosophy: provide the minimum necessary abstraction and let higher-level software handle complexity.

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `BMFS_BLOCK_SIZE` | 2 MiB (2,097,152 bytes) | Minimum allocation unit |
| `BMFS_MAX_FILES` | 64 | Maximum number of files on disk |
| `BMFS_MAX_FILENAME` | 32 | Maximum filename length in bytes (including null terminator) |
| `BMFS_ENTRY_SIZE` | 64 bytes | Size of one directory entry |

```c
#define BMFS_BLOCK_SIZE    (2 * 1024 * 1024)  // 2 MiB
#define BMFS_MAX_FILES     64
#define BMFS_MAX_FILENAME  32
#define BMFS_ENTRY_SIZE    64
```

## On-Disk Layout

```
Offset 0                                     Offset 4096 (0x1000)
+------------------------------------------+
| Directory Entries (64 x 64 bytes = 4 KB) |
+------------------------------------------+
| Block 0: File data (2 MB)                |
+------------------------------------------+
| Block 1: File data (2 MB)                |
+------------------------------------------+
| Block 2: File data (2 MB)                |
+------------------------------------------+
| ...                                      |
+------------------------------------------+
```

The first 4,096 bytes of the disk contain the directory table: 64 entries of 64 bytes each. File data begins immediately after, aligned to the 2MB block boundary. Each file occupies one or more contiguous 2MB blocks.

## Directory Entry Structure

```c
typedef struct __attribute__((packed)) {
    char     filename[32];       // Null-terminated filename (31 chars max + NUL)
    uint64_t starting_block;     // Index of the first 2MB block
    uint64_t reserved_blocks;    // Number of 2MB blocks allocated
    uint64_t file_size;          // Actual file size in bytes
    uint64_t unused;             // Reserved for future use
} bmfs_entry_t;
// sizeof(bmfs_entry_t) == 64 bytes
```

### Field Details

- **filename**: Up to 31 ASCII characters plus a null terminator. An entry with `filename[0] == 0x00` is considered empty (free slot). An entry with `filename[0] == 0x01` is considered deleted (tombstone, available for reuse).
- **starting_block**: Zero-indexed block number. The byte offset of the file data is `directory_size + (starting_block * BMFS_BLOCK_SIZE)`.
- **reserved_blocks**: The number of contiguous 2MB blocks reserved for this file. The file can grow up to `reserved_blocks * BMFS_BLOCK_SIZE` bytes without reallocation.
- **file_size**: The actual size of the file contents in bytes. Always less than or equal to `reserved_blocks * BMFS_BLOCK_SIZE`.
- **unused**: Reserved 8 bytes, currently set to zero. May be used for timestamps or attributes in a future version.

## API Reference

**Source**: `kernel/fs.c`, `kernel/fs.h`

### Initialization

```c
/**
 * Initialize the filesystem from a disk base address.
 * Reads the directory table and prepares internal state.
 *
 * @param disk_base  Pointer to the start of the BMFS disk image
 * @return           0 on success, -1 if the disk is not a valid BMFS volume
 */
int fs_init(void *disk_base);
```

### Listing Files

```c
/**
 * List all files on the filesystem.
 * Fills the entries array with directory entries for all non-empty,
 * non-deleted files.
 *
 * @param entries      Array to receive file entries
 * @param max_entries  Maximum number of entries to return
 * @return             Number of files found (0 to max_entries)
 */
int fs_list(bmfs_entry_t *entries, int max_entries);
```

### Opening Files

```c
/**
 * Open a file by name.
 * Returns a file descriptor (index into the directory table)
 * that can be used with fs_read() and fs_write().
 *
 * @param filename  Null-terminated filename to open
 * @return          File descriptor (>= 0) on success, -1 if not found
 */
int fs_open(const char *filename);
```

### Reading Files

```c
/**
 * Read data from an open file.
 *
 * @param fd      File descriptor from fs_open()
 * @param buffer  Destination buffer for file data
 * @param size    Maximum number of bytes to read
 * @return        Number of bytes actually read, or -1 on error
 */
int64_t fs_read(int fd, void *buffer, uint64_t size);
```

### Writing Files

```c
/**
 * Write data to an open file.
 * The file must have enough reserved blocks to hold the data.
 * Updates the file_size field in the directory entry.
 *
 * @param fd      File descriptor from fs_open()
 * @param buffer  Source buffer containing data to write
 * @param size    Number of bytes to write
 * @return        Number of bytes written, or -1 on error
 */
int64_t fs_write(int fd, const void *buffer, uint64_t size);
```

### Creating Files

```c
/**
 * Create a new file with the specified number of reserved blocks.
 * Finds a free directory entry and allocates contiguous blocks.
 *
 * @param filename        Null-terminated filename (max 31 characters)
 * @param initial_blocks  Number of 2MB blocks to reserve
 * @return                0 on success, -1 if no free entry or no contiguous space
 */
int fs_create(const char *filename, uint64_t initial_blocks);
```

### Deleting Files

```c
/**
 * Delete a file by name.
 * Marks the directory entry as deleted (tombstone) and frees the blocks.
 *
 * @param filename  Null-terminated filename to delete
 * @return          0 on success, -1 if file not found
 */
int fs_delete(const char *filename);
```

### File Information

```c
/**
 * Get file metadata without opening the file.
 *
 * @param filename  Null-terminated filename to query
 * @param stat      Pointer to a bmfs_stat_t structure to fill
 * @return          0 on success, -1 if file not found
 */
int fs_stat(const char *filename, bmfs_stat_t *stat);
```

The `bmfs_stat_t` structure contains:

```c
typedef struct {
    char     filename[32];
    uint64_t file_size;        // Actual file size in bytes
    uint64_t reserved_blocks;  // Number of allocated 2MB blocks
    uint64_t total_reserved;   // reserved_blocks * BMFS_BLOCK_SIZE
} bmfs_stat_t;
```

## Design Choices

### Flat Namespace

BMFS has no directories. All files exist in a single flat namespace. This eliminates the complexity of path parsing, directory traversal, and recursive operations. The AI chat engine handles user-facing file organization through natural language (e.g., "show my text files" filters by extension).

### Contiguous Allocation

Files are stored in contiguous 2MB blocks. This means:
- **No fragmentation**: Every file is a single contiguous region on disk
- **Fast sequential reads**: No need to follow block chains or extent trees
- **Simple addressing**: File offset = `disk_base + directory_size + (starting_block * BMFS_BLOCK_SIZE) + offset`

### Minimal Metadata

Each file has only a name, location, and size. There are no timestamps, no permissions, no ownership, and no extended attributes. This keeps the directory entry at exactly 64 bytes and the entire directory table at 4 KB.

## Limitations

| Limitation | Value | Rationale |
|-----------|-------|-----------|
| Maximum files | 64 | Fixed directory table size, no dynamic allocation |
| Minimum allocation | 2 MB per file | Large block size simplifies allocation |
| Maximum filename | 31 characters | Fixed-size entry, no variable-length names |
| No subdirectories | Flat only | Simplicity, exokernel philosophy |
| No permissions | None | Single-user OS, no multi-user access control |
| No journaling | None | Simplicity over crash recovery |
| No sparse files | Not supported | Contiguous allocation model |

These limitations are acceptable for AlJefra OS because the filesystem is used primarily for configuration files, driver packages, and user documents. The AI layer provides a richer abstraction when needed.
