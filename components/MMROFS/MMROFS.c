/*
 * MMROFS - Memory-Mapped Read-Only-ish File System
 *
 * Transactional, crash-safe filesystem for SPI NOR flash.
 * See MMROFS_FORMAT_SPEC_v2.md for full specification.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"

#include "MMROFS.h"

static const char *TAG = "MMROFS";

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define MMROFS_HEADER_SIZE         0x10000   /* 64 KB header region */
#define MMROFS_DATA_REGION_START   0x10000
#define MMROFS_ENTRY_SIZE          32
#define MMROFS_ERASE_BLOCK_SIZE    4096
#define MMROFS_MMAP_WINDOW_SIZE    0x10000   /* 64 KB sliding window */
#define MMROFS_MAX_FILENAME_LEN    255
#define MMROFS_ENTRIES_PER_PAGE    (MMROFS_ERASE_BLOCK_SIZE / MMROFS_ENTRY_SIZE) /* 128 */
#define MMROFS_HEADER_PAGES        (MMROFS_HEADER_SIZE / MMROFS_ERASE_BLOCK_SIZE) /* 16 */
#define MMROFS_MAX_ENTRIES         ((MMROFS_HEADER_SIZE - MMROFS_ERASE_BLOCK_SIZE) / MMROFS_ENTRY_SIZE) /* 1920, but spec says ~1875 usable */

/* Entry states */
#define STATE_FREE              0xFF
#define STATE_ALLOCATING        0x7F
#define STATE_PENDING_DATA      0x3F
#define STATE_TOMBSTONING_OLD   0x1F
#define STATE_ACTIVE            0x0F
#define STATE_VALID             0x07
#define STATE_TOMBSTONE         0x03
#define STATE_BADBLOCK          0x01
#define STATE_ERASED            0x00

/* Sentinel values */
#define ENTRY_NONE              0xFFFFFFFF
#define MTIME_UNSET             0xFFFFFFFF
#define SIZE_UNKNOWN             0xFFFFFFFF
#define CAPACITY_MASK_BITS      0xFFF

/* FD states */
#define FD_STATE_UNUSED         0
#define FD_STATE_PENDING_NEW    1
#define FD_STATE_PENDING_UPDATE 2
#define FD_STATE_COMMITTED      3

/* --------------------------------------------------------------------------
 * On-flash entry structure (32 bytes, little-endian, packed)
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  state;
    uint8_t  reserved_v;
    uint16_t name_len;
    uint32_t name_hash;
    uint32_t offset;
    uint32_t size;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t old_entry;
    uint32_t dst_entry;
} mmrofs_entry_t;

_Static_assert(sizeof(mmrofs_entry_t) == 32, "Entry must be 32 bytes");

/* --------------------------------------------------------------------------
 * File descriptor
 * -------------------------------------------------------------------------- */

typedef struct {
    bool     in_use;
    uint16_t entry_index;
    uint32_t name_hash;
    uint16_t name_len;
    char     filename[MMROFS_MAX_FILENAME_LEN + 1];
    uint32_t data_offset;     /* read cursor within file data */
    uint32_t flash_offset;    /* partition-relative offset of data region */
    uint32_t data_size;       /* read: entry.size - name_len. write: bytes written so far */
    uint8_t  flags;
    uint8_t  fd_state;
    uint16_t old_entry_index;
    uint32_t old_data_size;
    uint32_t old_ctime;
} mmrofs_fd_t;

/* --------------------------------------------------------------------------
 * Directory handle
 * -------------------------------------------------------------------------- */

typedef struct {
    bool     in_use;
    uint16_t scan_index;
} mmrofs_dir_t;

#define MMROFS_MAX_DIRS 2

/* --------------------------------------------------------------------------
 * Filesystem context
 * -------------------------------------------------------------------------- */

typedef struct {
    const esp_partition_t *partition;
    SemaphoreHandle_t      mutex;
    mmrofs_fd_t           *fds;
    int                    max_files;
    uint16_t               next_free_entry;
    uint16_t               max_entries;
    uint32_t               partition_size;

    /* mmap state for header reads (permanent, covers full 64 KB header) */
    spi_flash_mmap_handle_t header_mmap_handle;
    const void             *header_mmap_ptr;

    /* mmap state for data reads (sliding 64 KB window) */
    spi_flash_mmap_handle_t mmap_handle;
    const void             *mmap_ptr;
    uint32_t                mmap_offset;  /* current mapped window start (partition-relative) */
    bool                    mmap_valid;

    /* directory handles */
    mmrofs_dir_t            dirs[MMROFS_MAX_DIRS];
} mmrofs_t;

/* We only support a single mount at a time */
static mmrofs_t *s_mmrofs = NULL;

/* --------------------------------------------------------------------------
 * Utility: FNV-1a 32-bit hash
 * -------------------------------------------------------------------------- */

static uint32_t fnv1a32(const char *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* --------------------------------------------------------------------------
 * Low-level flash helpers
 * -------------------------------------------------------------------------- */

static esp_err_t flash_write(const mmrofs_t *fs, uint32_t offset, const void *buf, size_t len)
{
    return esp_partition_write(fs->partition, offset, buf, len);
}

static esp_err_t flash_erase(const mmrofs_t *fs, uint32_t offset, size_t len)
{
    return esp_partition_erase_range(fs->partition, offset, len);
}

/* --------------------------------------------------------------------------
 * Entry read/write helpers
 * -------------------------------------------------------------------------- */

static uint32_t entry_flash_offset(uint16_t index)
{
    return (uint32_t)index * MMROFS_ENTRY_SIZE;
}

static esp_err_t entry_read(const mmrofs_t *fs, uint16_t index, mmrofs_entry_t *e)
{
    uint32_t offset = entry_flash_offset(index);
    memcpy(e, (const uint8_t *)fs->header_mmap_ptr + offset, sizeof(*e));
    return ESP_OK;
}

static esp_err_t entry_write_state(const mmrofs_t *fs, uint16_t index, uint8_t state)
{
    return flash_write(fs, entry_flash_offset(index), &state, 1);
}

static esp_err_t entry_write_full(const mmrofs_t *fs, uint16_t index, const mmrofs_entry_t *e)
{
    /* Write the full 32-byte entry. State byte should already be ALLOCATING. */
    return flash_write(fs, entry_flash_offset(index), e, sizeof(*e));
}

static esp_err_t entry_write_field(const mmrofs_t *fs, uint16_t index,
                                   size_t field_offset, const void *val, size_t len)
{
    return flash_write(fs, entry_flash_offset(index) + field_offset, val, len);
}

/* Write mtime field (offset 16) */
static esp_err_t entry_write_mtime(const mmrofs_t *fs, uint16_t index, uint32_t mtime)
{
    return entry_write_field(fs, index, offsetof(mmrofs_entry_t, mtime), &mtime, sizeof(mtime));
}

/* Write size field (offset 12) */
static esp_err_t entry_write_size(const mmrofs_t *fs, uint16_t index, uint32_t size)
{
    return entry_write_field(fs, index, offsetof(mmrofs_entry_t, size), &size, sizeof(size));
}

/* --------------------------------------------------------------------------
 * Allocation helpers
 * -------------------------------------------------------------------------- */

static uint32_t align_up_4k(uint32_t val)
{
    return (val + MMROFS_ERASE_BLOCK_SIZE - 1) & ~(MMROFS_ERASE_BLOCK_SIZE - 1);
}

static uint32_t entry_allocated_bytes(const mmrofs_entry_t *e)
{
    return align_up_4k(e->size);
}

static bool is_capacity_mask(uint32_t size)
{
    return (size & CAPACITY_MASK_BITS) == CAPACITY_MASK_BITS;
}

/* Check if an entry is visible (ACTIVE or VALID) */
static bool entry_is_live(uint8_t state)
{
    return state == STATE_ACTIVE || state == STATE_VALID;
}

/* --------------------------------------------------------------------------
 * Entry validation (per spec)
 * -------------------------------------------------------------------------- */

static bool entry_validate(const mmrofs_t *fs, const mmrofs_entry_t *e)
{
    if (e->name_len == 0 || e->name_len > MMROFS_MAX_FILENAME_LEN)
        return false;
    if (e->size < e->name_len)
        return false;
    if (e->offset < MMROFS_DATA_REGION_START)
        return false;
    if ((e->offset % MMROFS_ERASE_BLOCK_SIZE) != 0)
        return false;
    uint32_t alloc = entry_allocated_bytes(e);
    if (e->offset + alloc > fs->partition_size)
        return false;
    if (e->old_entry != ENTRY_NONE && e->old_entry >= fs->max_entries)
        return false;
    if (e->dst_entry != ENTRY_NONE && e->dst_entry >= fs->max_entries)
        return false;
    return true;
}

/* --------------------------------------------------------------------------
 * Mmap window management for data reads
 * -------------------------------------------------------------------------- */

static esp_err_t mmrofs_map_window(mmrofs_t *fs, uint32_t offset)
{
    /* Align to 64 KB window boundary */
    uint32_t window_start = offset & ~(MMROFS_MMAP_WINDOW_SIZE - 1);

    if (fs->mmap_valid && fs->mmap_offset == window_start) {
        return ESP_OK; /* Already mapped */
    }

    if (fs->mmap_valid) {
        spi_flash_munmap(fs->mmap_handle);
        fs->mmap_valid = false;
    }

    esp_err_t ret = esp_partition_mmap(fs->partition, window_start, MMROFS_MMAP_WINDOW_SIZE,
                                       ESP_PARTITION_MMAP_DATA, &fs->mmap_ptr, &fs->mmap_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    fs->mmap_offset = window_start;
    fs->mmap_valid = true;
    return ESP_OK;
}

/* Read from data region via mmap. Falls back to partition_read for cross-window reads. */
static esp_err_t mmrofs_data_read(mmrofs_t *fs, uint32_t offset, void *buf, size_t len)
{
    /* Check if entirely within one window */
    uint32_t window_start = offset & ~(MMROFS_MMAP_WINDOW_SIZE - 1);
    uint32_t window_end_exclusive = window_start + MMROFS_MMAP_WINDOW_SIZE;

    if (offset + len <= window_end_exclusive) {
        esp_err_t ret = mmrofs_map_window(fs, offset);
        if (ret != ESP_OK) {
            return ret;
        }
        uint32_t off_in_window = offset - fs->mmap_offset;
        memcpy(buf, (const uint8_t *)fs->mmap_ptr + off_in_window, len);
        return ESP_OK;
    }

    /* Cross-window: read in two parts via mmap */
    size_t first_len = window_end_exclusive - offset;
    esp_err_t ret = mmrofs_map_window(fs, offset);
    if (ret != ESP_OK) return ret;
    memcpy(buf, (const uint8_t *)fs->mmap_ptr + (offset - fs->mmap_offset), first_len);

    ret = mmrofs_map_window(fs, window_end_exclusive);
    if (ret != ESP_OK) return ret;
    memcpy((uint8_t *)buf + first_len, (const uint8_t *)fs->mmap_ptr, len - first_len);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Mutex helpers
 * -------------------------------------------------------------------------- */

static void mmrofs_lock(mmrofs_t *fs)
{
    xSemaphoreTakeRecursive(fs->mutex, portMAX_DELAY);
}

static void mmrofs_unlock(mmrofs_t *fs)
{
    xSemaphoreGiveRecursive(fs->mutex);
}

/* --------------------------------------------------------------------------
 * File lookup: scan entire entry table for VALID/ACTIVE match
 * -------------------------------------------------------------------------- */

static int mmrofs_lookup(mmrofs_t *fs, const char *name, uint16_t name_len,
                          uint32_t hash, mmrofs_entry_t *out_entry)
{
    mmrofs_entry_t e;
    char stored_name[MMROFS_MAX_FILENAME_LEN];

    for (uint16_t i = 0; i < fs->max_entries; i++) {
        if (entry_read(fs, i, &e) != ESP_OK)
            continue;
        if (!entry_is_live(e.state))
            continue;
        if (e.name_hash != hash)
            continue;
        if (e.name_len != name_len)
            continue;

        /* Validate before reading data region */
        if (!entry_validate(fs, &e)) {
            entry_write_state(fs, i, STATE_TOMBSTONE);
            continue;
        }

        /* Read stored filename */
        if (mmrofs_data_read(fs, e.offset, stored_name, name_len) != ESP_OK)
            continue;

        if (memcmp(stored_name, name, name_len) == 0) {
            if (out_entry)
                *out_entry = e;
            return (int)i;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Entry allocation: find a FREE slot
 * -------------------------------------------------------------------------- */

static int mmrofs_alloc_entry(mmrofs_t *fs)
{
    for (uint16_t i = fs->next_free_entry; i < fs->max_entries; i++) {
        const uint8_t *slot_ptr = (const uint8_t *)fs->header_mmap_ptr + entry_flash_offset(i);

        if (slot_ptr[0] != STATE_FREE)
            continue;

        /* Verify entire slot is 0xFF */
        bool all_ff = true;
        for (int j = 0; j < MMROFS_ENTRY_SIZE; j++) {
            if (slot_ptr[j] != 0xFF) {
                all_ff = false;
                break;
            }
        }

        if (!all_ff) {
            /* Corrupted FREE slot */
            entry_write_state(fs, i, STATE_ERASED);
            continue;
        }

        fs->next_free_entry = i + 1;
        return (int)i;
    }

    return -1; /* No free slot */
}

/* --------------------------------------------------------------------------
 * Data region allocation: find contiguous free space
 * -------------------------------------------------------------------------- */

/* Build a list of occupied ranges (offset, end) from live entries */
static int mmrofs_find_free_data(mmrofs_t *fs, uint32_t needed_bytes, uint32_t *out_offset)
{
    /* Scan all live entries to find the end of used data region */
    mmrofs_entry_t e;
    uint32_t data_end = MMROFS_DATA_REGION_START;

    for (uint16_t i = 0; i < fs->max_entries; i++) {
        if (entry_read(fs, i, &e) != ESP_OK)
            continue;
        if (!entry_is_live(e.state) && e.state != STATE_TOMBSTONING_OLD)
            continue;
        if (e.offset < MMROFS_DATA_REGION_START)
            continue;

        uint32_t end = e.offset + entry_allocated_bytes(&e);
        if (end > data_end)
            data_end = end;
    }

    /* Simple: allocate at the end of all live data */
    uint32_t candidate = align_up_4k(data_end);
    uint32_t alloc = align_up_4k(needed_bytes);

    if (candidate + alloc > fs->partition_size) {
        /* Try to find gaps between files */
        /* For now, simple first-fit from beginning of data region */
        candidate = MMROFS_DATA_REGION_START;

        /* Collect all live file ranges and sort by offset (simple bubble sort, <50 files) */
        typedef struct { uint32_t start; uint32_t end; } range_t;
        range_t *ranges = calloc(fs->max_entries, sizeof(range_t));
        if (!ranges)
            return -1;

        int range_count = 0;
        for (uint16_t i = 0; i < fs->max_entries; i++) {
            if (entry_read(fs, i, &e) != ESP_OK)
                continue;
            if (!entry_is_live(e.state) && e.state != STATE_TOMBSTONING_OLD)
                continue;
            if (e.offset < MMROFS_DATA_REGION_START)
                continue;
            ranges[range_count].start = e.offset;
            ranges[range_count].end = e.offset + entry_allocated_bytes(&e);
            range_count++;
        }

        /* Sort by start offset */
        for (int a = 0; a < range_count - 1; a++) {
            for (int b = a + 1; b < range_count; b++) {
                if (ranges[b].start < ranges[a].start) {
                    range_t tmp = ranges[a];
                    ranges[a] = ranges[b];
                    ranges[b] = tmp;
                }
            }
        }

        /* Find first gap that fits */
        candidate = MMROFS_DATA_REGION_START;
        bool found = false;
        for (int r = 0; r < range_count; r++) {
            if (candidate + alloc <= ranges[r].start) {
                found = true;
                break;
            }
            if (ranges[r].end > candidate)
                candidate = ranges[r].end;
        }
        if (!found) {
            /* Check space after last file */
            if (candidate + alloc > fs->partition_size) {
                free(ranges);
                return -1; /* ENOSPC */
            }
        }
        free(ranges);
    }

    *out_offset = candidate;
    return 0;
}

/* --------------------------------------------------------------------------
 * Check if space after an existing file is free for append
 * -------------------------------------------------------------------------- */

static bool mmrofs_space_after_free(mmrofs_t *fs, uint32_t offset, uint32_t current_alloc,
                                     uint32_t needed_total)
{
    uint32_t new_alloc = align_up_4k(needed_total);
    if (new_alloc <= current_alloc)
        return true; /* Already fits */

    uint32_t extra_start = offset + current_alloc;
    uint32_t extra_end = offset + new_alloc;

    if (extra_end > fs->partition_size)
        return false;

    /* Check no live entry overlaps with [extra_start, extra_end) */
    mmrofs_entry_t e;
    for (uint16_t i = 0; i < fs->max_entries; i++) {
        if (entry_read(fs, i, &e) != ESP_OK)
            continue;
        if (!entry_is_live(e.state))
            continue;
        if (e.offset < MMROFS_DATA_REGION_START)
            continue;

        uint32_t file_end = e.offset + entry_allocated_bytes(&e);
        /* Overlaps? */
        if (e.offset < extra_end && file_end > extra_start)
            return false;
    }

    /* Verify the flash is erased */
    uint8_t buf[64];
    for (uint32_t pos = extra_start; pos < extra_end; pos += sizeof(buf)) {
        size_t chunk = sizeof(buf);
        if (pos + chunk > extra_end)
            chunk = extra_end - pos;
        if (mmrofs_data_read(fs, pos, buf, chunk) != ESP_OK)
            return false;
        for (size_t j = 0; j < chunk; j++) {
            if (buf[j] != 0xFF)
                return false;
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * Allocate and populate a new entry (ALLOCATING → PENDING_DATA)
 * Writes all metadata fields and transitions through ALLOCATING → PENDING_DATA.
 * Returns the slot index, or -1 on failure.
 * -------------------------------------------------------------------------- */

static int mmrofs_create_entry(mmrofs_t *fs, uint16_t name_len, uint32_t name_hash,
                                uint32_t offset, uint32_t size, uint32_t ctime,
                                uint32_t old_entry, uint32_t dst_entry)
{
    int slot = mmrofs_alloc_entry(fs);
    if (slot < 0) {
        /* TODO: entry table defragmentation */
        ESP_LOGE(TAG, "No free entry slot");
        return -1;
    }

    /* Write ALLOCATING state first */
    esp_err_t ret = entry_write_state(fs, slot, STATE_ALLOCATING);
    if (ret != ESP_OK) return -1;

    /* Populate entry metadata (state already written, skip byte 0) */
    mmrofs_entry_t e = {
        .state = STATE_ALLOCATING,
        .reserved_v = 0x00,
        .name_len = name_len,
        .name_hash = name_hash,
        .offset = offset,
        .size = size,
        .mtime = MTIME_UNSET,
        .ctime = ctime,
        .old_entry = old_entry,
        .dst_entry = dst_entry,
    };

    /* Write remaining fields (bytes 1-31) after state byte */
    ret = flash_write(fs, entry_flash_offset(slot) + 1,
                      ((const uint8_t *)&e) + 1,
                      sizeof(e) - 1);
    if (ret != ESP_OK) {
        entry_write_state(fs, slot, STATE_TOMBSTONE);
        return -1;
    }

    /* Transition to PENDING_DATA */
    ret = entry_write_state(fs, slot, STATE_PENDING_DATA);
    if (ret != ESP_OK) {
        entry_write_state(fs, slot, STATE_TOMBSTONE);
        return -1;
    }

    return slot;
}

/* --------------------------------------------------------------------------
 * Tombstone-old flow: PENDING_DATA → TOMBSTONING_OLD → tombstone old → ACTIVE
 * -------------------------------------------------------------------------- */

static esp_err_t mmrofs_tombstone_old_flow(mmrofs_t *fs, uint16_t new_slot,
                                            uint16_t old_slot, uint32_t dst_slot_val)
{
    esp_err_t ret;

    /* PENDING_DATA → TOMBSTONING_OLD */
    ret = entry_write_state(fs, new_slot, STATE_TOMBSTONING_OLD);
    if (ret != ESP_OK) return ret;

    /* Tombstone old entry */
    ret = entry_write_state(fs, old_slot, STATE_TOMBSTONE);
    if (ret != ESP_OK) return ret;

    /* Tombstone dst entry if applicable */
    if (dst_slot_val != ENTRY_NONE && dst_slot_val < fs->max_entries) {
        ret = entry_write_state(fs, (uint16_t)dst_slot_val, STATE_TOMBSTONE);
        if (ret != ESP_OK) return ret;
    }

    /* TOMBSTONING_OLD → ACTIVE */
    ret = entry_write_state(fs, new_slot, STATE_ACTIVE);
    return ret;
}

/* --------------------------------------------------------------------------
 * FD helpers
 * -------------------------------------------------------------------------- */

static int mmrofs_alloc_fd(mmrofs_t *fs)
{
    for (int i = 0; i < fs->max_files; i++) {
        if (!fs->fds[i].in_use) {
            memset(&fs->fds[i], 0, sizeof(mmrofs_fd_t));
            fs->fds[i].in_use = true;
            fs->fds[i].entry_index = 0xFFFF;
            return i;
        }
    }
    return -1;
}

static mmrofs_fd_t *mmrofs_get_fd(mmrofs_t *fs, int fd)
{
    if (fd < 0 || fd >= fs->max_files)
        return NULL;
    if (!fs->fds[fd].in_use)
        return NULL;
    return &fs->fds[fd];
}

static void mmrofs_free_fd(mmrofs_t *fs, int fd)
{
    if (fd >= 0 && fd < fs->max_files) {
        fs->fds[fd].in_use = false;
    }
}

/* --------------------------------------------------------------------------
 * Recovery: boot-time scan
 * -------------------------------------------------------------------------- */

static void mmrofs_recover(mmrofs_t *fs)
{
    mmrofs_entry_t e;
    uint8_t raw[MMROFS_ENTRY_SIZE];
    uint16_t first_free = fs->max_entries;

    for (uint16_t i = 0; i < fs->max_entries; i++) {
        const uint8_t *raw_ptr = (const uint8_t *)fs->header_mmap_ptr + entry_flash_offset(i);
        memcpy(raw, raw_ptr, MMROFS_ENTRY_SIZE);

        uint8_t state = raw[0];
        memcpy(&e, raw, sizeof(e));

        switch (state) {
        case STATE_FREE: {
            /* Check entire slot is 0xFF */
            bool all_ff = true;
            for (int j = 0; j < MMROFS_ENTRY_SIZE; j++) {
                if (raw[j] != 0xFF) { all_ff = false; break; }
            }
            if (!all_ff) {
                entry_write_state(fs, i, STATE_ERASED);
            } else {
                if (i < first_free) first_free = i;
            }
            break;
        }

        case STATE_ALLOCATING:
        case STATE_PENDING_DATA:
            ESP_LOGW(TAG, "Recovery: tombstoning incomplete entry %u (state=0x%02X)", i, state);
            entry_write_state(fs, i, STATE_TOMBSTONE);
            break;

        case STATE_TOMBSTONING_OLD:
            ESP_LOGI(TAG, "Recovery: completing TOMBSTONING_OLD for entry %u", i);
            if (!entry_validate(fs, &e)) {
                entry_write_state(fs, i, STATE_TOMBSTONE);
                break;
            }

            /* Tombstone old_entry if it's live */
            if (e.old_entry != ENTRY_NONE && e.old_entry < fs->max_entries) {
                mmrofs_entry_t old;
                if (entry_read(fs, (uint16_t)e.old_entry, &old) == ESP_OK) {
                    if (entry_is_live(old.state)) {
                        entry_write_state(fs, (uint16_t)e.old_entry, STATE_TOMBSTONE);
                    }
                }
            }

            /* Tombstone dst_entry if it's live */
            if (e.dst_entry != ENTRY_NONE && e.dst_entry < fs->max_entries) {
                mmrofs_entry_t dst;
                if (entry_read(fs, (uint16_t)e.dst_entry, &dst) == ESP_OK) {
                    if (entry_is_live(dst.state)) {
                        entry_write_state(fs, (uint16_t)e.dst_entry, STATE_TOMBSTONE);
                    }
                }
            }

            /* Transition to ACTIVE, then fall through to ACTIVE recovery */
            entry_write_state(fs, i, STATE_ACTIVE);
            /* Re-read after state change */
            entry_read(fs, i, &e);
            goto active_recovery;

        case STATE_ACTIVE:
active_recovery:
            if (!entry_validate(fs, &e)) {
                entry_write_state(fs, i, STATE_TOMBSTONE);
                break;
            }

            ESP_LOGI(TAG, "Recovery: processing ACTIVE entry %u", i);

            /* Size recovery: always scan backwards for trailing 0xFF */
            {
                uint32_t alloc_bytes = entry_allocated_bytes(&e);
                uint32_t scan_end = e.offset + alloc_bytes;
                uint32_t last_non_ff = e.offset + e.name_len; /* minimum: after filename */
                uint8_t buf[64];

                /* Scan backwards in chunks */
                uint32_t pos = scan_end;
                bool found_data = false;
                while (pos > e.offset + e.name_len && !found_data) {
                    uint32_t chunk_start = (pos > sizeof(buf)) ? (pos - sizeof(buf)) : e.offset;
                    if (chunk_start < e.offset + e.name_len)
                        chunk_start = e.offset + e.name_len;
                    size_t chunk_len = pos - chunk_start;

                    if (mmrofs_data_read(fs, chunk_start, buf, chunk_len) != ESP_OK)
                        break;

                    for (int j = (int)chunk_len - 1; j >= 0; j--) {
                        if (buf[j] != 0xFF) {
                            last_non_ff = chunk_start + j;
                            found_data = true;
                            break;
                        }
                    }
                    pos = chunk_start;
                }

                uint32_t inferred_size;
                if (found_data) {
                    inferred_size = (last_non_ff - e.offset) + 1;
                } else {
                    inferred_size = e.name_len; /* No data, just filename */
                }

                if (is_capacity_mask(e.size)) {
                    /* Capacity mask — write exact size */
                    if (inferred_size != e.size) {
                        entry_write_size(fs, i, inferred_size);
                        e.size = inferred_size;
                    }
                } else if (inferred_size < e.size) {
                    uint32_t trailing_ff = e.size - inferred_size;
                    if (trailing_ff > 2) {
                        /* Likely torn write */
                        entry_write_size(fs, i, inferred_size);
                        e.size = inferred_size;
                    }
                }
            }

            /* Mtime recovery */
            if (e.mtime == MTIME_UNSET) {
                /* Never programmed — safe to write in-place */
                uint32_t now = (uint32_t)time(NULL);
                entry_write_mtime(fs, i, now);
                entry_write_state(fs, i, STATE_VALID);
                ESP_LOGI(TAG, "Recovery: promoted entry %u to VALID (mtime set)", i);
            } else {
                /* Could be valid or torn — must allocate new entry */
                int new_slot = mmrofs_alloc_entry(fs);
                if (new_slot >= 0) {
                    uint32_t now = (uint32_t)time(NULL);

                    entry_write_state(fs, new_slot, STATE_ALLOCATING);

                    mmrofs_entry_t ne = {
                        .state = STATE_ALLOCATING,
                        .reserved_v = 0x00,
                        .name_len = e.name_len,
                        .name_hash = e.name_hash,
                        .offset = e.offset,
                        .size = e.size,
                        .mtime = now,
                        .ctime = e.ctime,
                        .old_entry = i,
                        .dst_entry = ENTRY_NONE,
                    };

                    flash_write(fs, entry_flash_offset(new_slot) + 1,
                               ((const uint8_t *)&ne) + 1, sizeof(ne) - 1);
                    entry_write_state(fs, new_slot, STATE_PENDING_DATA);
                    entry_write_state(fs, new_slot, STATE_TOMBSTONING_OLD);
                    entry_write_state(fs, i, STATE_TOMBSTONE);
                    entry_write_state(fs, new_slot, STATE_ACTIVE);
                    /* mtime was written by us this boot — trust it, promote to VALID */
                    entry_write_state(fs, new_slot, STATE_VALID);
                    ESP_LOGI(TAG, "Recovery: re-allocated entry %u → %d (torn mtime)", i, new_slot);
                } else {
                    /* Cannot allocate — leave as ACTIVE, mtime might be wrong */
                    ESP_LOGW(TAG, "Recovery: cannot fix torn mtime for entry %u (no free slots)", i);
                }
            }
            break;

        case STATE_VALID:
            if (!entry_validate(fs, &e)) {
                ESP_LOGW(TAG, "Recovery: tombstoning invalid VALID entry %u", i);
                entry_write_state(fs, i, STATE_TOMBSTONE);
            }
            break;

        case STATE_TOMBSTONE:
        case STATE_BADBLOCK:
        case STATE_ERASED:
            /* Leave as-is */
            break;

        default:
            /* Unknown state — could be partially written, mark erased */
            ESP_LOGW(TAG, "Recovery: unknown state 0x%02X at entry %u, marking ERASED", state, i);
            entry_write_state(fs, i, STATE_ERASED);
            break;
        }
    }

    /* Rebuild next_free_entry */
    fs->next_free_entry = first_free;
}

/* --------------------------------------------------------------------------
 * Boot-time clock initialization
 * -------------------------------------------------------------------------- */

static void mmrofs_init_clock(mmrofs_t *fs)
{
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);

    if (t.tm_year + 1900 < 1990) {
        /* System time invalid — scan entries for max mtime/ctime */
        uint32_t max_time = 0;
        mmrofs_entry_t e;

        for (uint16_t i = 0; i < fs->max_entries; i++) {
            if (entry_read(fs, i, &e) != ESP_OK) continue;
            if (!entry_is_live(e.state)) continue;
            if (e.mtime != MTIME_UNSET && e.mtime > max_time) max_time = e.mtime;
            if (e.ctime > max_time) max_time = e.ctime;
        }

        if (max_time > 0) {
            struct timeval tv = { .tv_sec = max_time, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Clock initialized from filesystem: %lu", (unsigned long)max_time);
        }
    }
}

/* --------------------------------------------------------------------------
 * VFS callbacks
 * -------------------------------------------------------------------------- */

static int vfs_open(void *ctx, const char *path, int flags, int mode);
static ssize_t vfs_write(void *ctx, int fd, const void *data, size_t size);
static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size);
static int vfs_close(void *ctx, int fd);
static int vfs_fstat(void *ctx, int fd, struct stat *st);
static int vfs_stat(void *ctx, const char *path, struct stat *st);
static off_t vfs_lseek(void *ctx, int fd, off_t offset, int whence);
static int vfs_unlink(void *ctx, const char *path);
static int vfs_rename(void *ctx, const char *src, const char *dst);
static DIR *vfs_opendir(void *ctx, const char *path);
static struct dirent *vfs_readdir(void *ctx, DIR *pdir);
static int vfs_closedir(void *ctx, DIR *pdir);

/* ---- open ---- */

static int vfs_open(void *ctx, const char *path, int flags, int mode)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;

    /* Strip leading '/' */
    if (path[0] == '/') path++;
    uint16_t name_len = (uint16_t)strlen(path);
    if (name_len == 0 || name_len > MMROFS_MAX_FILENAME_LEN) {
        errno = EINVAL;
        return -1;
    }

    uint32_t hash = fnv1a32(path, name_len);

    mmrofs_lock(fs);

    /* Lookup existing file */
    mmrofs_entry_t existing;
    int existing_idx = mmrofs_lookup(fs, path, name_len, hash, &existing);

    /* Allocate FD */
    int fd = mmrofs_alloc_fd(fs);
    if (fd < 0) {
        mmrofs_unlock(fs);
        errno = ENFILE;
        return -1;
    }

    mmrofs_fd_t *f = &fs->fds[fd];
    f->name_hash = hash;
    f->name_len = name_len;
    memcpy(f->filename, path, name_len);
    f->filename[name_len] = '\0';
    f->flags = (uint8_t)flags;
    f->data_offset = 0;

    if ((flags & O_ACCMODE) == O_RDONLY) {
        /* Read-only: file must exist */
        if (existing_idx < 0) {
            mmrofs_free_fd(fs, fd);
            mmrofs_unlock(fs);
            errno = ENOENT;
            return -1;
        }
        f->entry_index = (uint16_t)existing_idx;
        f->flash_offset = existing.offset;
        f->data_size = existing.size - existing.name_len;
        f->fd_state = FD_STATE_COMMITTED;
        mmrofs_unlock(fs);
        return fd;
    }

    /* Write modes */
    if (existing_idx >= 0) {
        f->fd_state = FD_STATE_PENDING_UPDATE;
        f->old_entry_index = (uint16_t)existing_idx;
        f->old_ctime = existing.ctime;

        if (entry_is_live(existing.state) && !is_capacity_mask(existing.size)) {
            f->old_data_size = existing.size - existing.name_len;
        } else {
            f->old_data_size = SIZE_UNKNOWN;
        }
        f->flash_offset = existing.offset;
    } else {
        /* File does not exist */
        if (!(flags & O_CREAT)) {
            mmrofs_free_fd(fs, fd);
            mmrofs_unlock(fs);
            errno = ENOENT;
            return -1;
        }
        f->fd_state = FD_STATE_PENDING_NEW;
        f->old_data_size = 0;
    }

    mmrofs_unlock(fs);
    return fd;
}

/* ---- write ---- */

static ssize_t vfs_write(void *ctx, int fd_num, const void *data, size_t size)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    mmrofs_fd_t *f = mmrofs_get_fd(fs, fd_num);
    if (!f) { errno = EBADF; return -1; }
    if ((f->flags & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -1; }
    if (size == 0) return 0;

    mmrofs_lock(fs);

    if (f->fd_state == FD_STATE_PENDING_NEW) {
        /* --- First write: new file --- */
        uint32_t total_size = (uint32_t)f->name_len + (uint32_t)size;
        uint32_t alloc_size = align_up_4k(total_size);
        uint32_t capacity_mask = alloc_size - 1; /* all low bits 1 — safe for later 1→0 size finalize */

        uint32_t data_offset;
        if (mmrofs_find_free_data(fs, alloc_size, &data_offset) < 0) {
            mmrofs_unlock(fs);
            errno = ENOSPC;
            return -1;
        }

        /* Erase target blocks if needed */
        flash_erase(fs, data_offset, alloc_size);

        uint32_t ctime_val = (uint32_t)time(NULL);

        int slot = mmrofs_create_entry(fs, f->name_len, f->name_hash,
                                        data_offset, capacity_mask, ctime_val,
                                        ENTRY_NONE, ENTRY_NONE);
        if (slot < 0) {
            mmrofs_unlock(fs);
            errno = ENOSPC;
            return -1;
        }

        /* Write filename */
        esp_err_t ret = flash_write(fs, data_offset, f->filename, f->name_len);
        if (ret != ESP_OK) {
            entry_write_state(fs, slot, STATE_TOMBSTONE);
            mmrofs_unlock(fs);
            errno = EIO;
            return -1;
        }

        /* Write file data */
        ret = flash_write(fs, data_offset + f->name_len, data, size);
        if (ret != ESP_OK) {
            entry_write_state(fs, slot, STATE_TOMBSTONE);
            mmrofs_unlock(fs);
            errno = EIO;
            return -1;
        }

        /* PENDING_DATA → ACTIVE */
        entry_write_state(fs, slot, STATE_ACTIVE);

        f->entry_index = (uint16_t)slot;
        f->flash_offset = data_offset;
        f->data_size = (uint32_t)size;
        f->fd_state = FD_STATE_COMMITTED;

        mmrofs_unlock(fs);
        return (ssize_t)size;

    } else if (f->fd_state == FD_STATE_PENDING_UPDATE) {
        /* --- First write: update existing file --- */
        if (f->old_data_size == SIZE_UNKNOWN) {
            mmrofs_unlock(fs);
            errno = EIO;
            return -1;
        }

        mmrofs_entry_t old_entry;
        if (entry_read(fs, f->old_entry_index, &old_entry) != ESP_OK) {
            mmrofs_unlock(fs);
            errno = EIO;
            return -1;
        }

        uint32_t new_total = (uint32_t)f->name_len + f->old_data_size + (uint32_t)size;
        uint32_t old_alloc = entry_allocated_bytes(&old_entry);

        /* Try append in-place */
        if (mmrofs_space_after_free(fs, old_entry.offset, old_alloc, new_total)) {
            /* Append flow */
            uint32_t new_alloc = align_up_4k(new_total);
            uint32_t capacity_mask = new_alloc - 1; /* all low bits 1 — safe for later 1→0 size finalize */

            /* Erase any new blocks needed */
            if (new_alloc > old_alloc) {
                flash_erase(fs, old_entry.offset + old_alloc, new_alloc - old_alloc);
            }

            int slot = mmrofs_create_entry(fs, f->name_len, f->name_hash,
                                            old_entry.offset, capacity_mask,
                                            f->old_ctime, f->old_entry_index, ENTRY_NONE);
            if (slot < 0) {
                mmrofs_unlock(fs);
                errno = ENOSPC;
                return -1;
            }

            /* Write new data after existing data */
            uint32_t write_pos = old_entry.offset + f->name_len + f->old_data_size;
            flash_write(fs, write_pos, data, size);

            /* TOMBSTONING_OLD flow */
            mmrofs_tombstone_old_flow(fs, (uint16_t)slot, f->old_entry_index, ENTRY_NONE);

            f->entry_index = (uint16_t)slot;
            f->flash_offset = old_entry.offset;
            f->data_size = f->old_data_size + (uint32_t)size;
            f->fd_state = FD_STATE_COMMITTED;

        } else {
            /* Full rewrite to new location */
            uint32_t new_alloc = align_up_4k(new_total);
            uint32_t capacity_mask = new_alloc - 1; /* all low bits 1 — safe for later 1→0 size finalize */
            uint32_t new_offset;
            if (mmrofs_find_free_data(fs, new_alloc, &new_offset) < 0) {
                mmrofs_unlock(fs);
                errno = ENOSPC;
                return -1;
            }

            flash_erase(fs, new_offset, new_alloc);

            int slot = mmrofs_create_entry(fs, f->name_len, f->name_hash,
                                            new_offset, capacity_mask,
                                            f->old_ctime, f->old_entry_index, ENTRY_NONE);
            if (slot < 0) {
                mmrofs_unlock(fs);
                errno = ENOSPC;
                return -1;
            }

            /* Write filename */
            flash_write(fs, new_offset, f->filename, f->name_len);

            /* Copy old data */
            if (f->old_data_size > 0) {
                uint8_t copy_buf[256];
                uint32_t old_data_start = old_entry.offset + old_entry.name_len;
                uint32_t remaining = f->old_data_size;
                uint32_t dst_pos = new_offset + f->name_len;

                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(copy_buf) ? sizeof(copy_buf) : remaining;
                    mmrofs_data_read(fs, old_data_start, copy_buf, chunk);
                    flash_write(fs, dst_pos, copy_buf, chunk);
                    old_data_start += chunk;
                    dst_pos += chunk;
                    remaining -= chunk;
                }
            }

            /* Write new data */
            flash_write(fs, new_offset + f->name_len + f->old_data_size, data, size);

            /* TOMBSTONING_OLD flow */
            mmrofs_tombstone_old_flow(fs, (uint16_t)slot, f->old_entry_index, ENTRY_NONE);

            f->entry_index = (uint16_t)slot;
            f->flash_offset = new_offset;
            f->data_size = f->old_data_size + (uint32_t)size;
            f->fd_state = FD_STATE_COMMITTED;
        }

        mmrofs_unlock(fs);
        return (ssize_t)size;

    } else if (f->fd_state == FD_STATE_COMMITTED) {
        /* --- Subsequent writes --- */
        mmrofs_entry_t cur;
        if (entry_read(fs, f->entry_index, &cur) != ESP_OK) {
            mmrofs_unlock(fs);
            errno = EIO;
            return -1;
        }

        uint32_t new_data_total = f->data_size + (uint32_t)size;
        uint32_t new_total = (uint32_t)f->name_len + new_data_total;
        uint32_t cur_alloc = entry_allocated_bytes(&cur);

        if (new_total <= cur_alloc) {
            /* Fits in current allocation */
            uint32_t write_pos = f->flash_offset + f->name_len + f->data_size;
            esp_err_t ret = flash_write(fs, write_pos, data, size);
            if (ret != ESP_OK) {
                mmrofs_unlock(fs);
                errno = EIO;
                return -1;
            }
            f->data_size = new_data_total;
        } else {
            /* Capacity expansion needed */
            uint32_t new_capacity = new_total | CAPACITY_MASK_BITS;

            if (mmrofs_space_after_free(fs, f->flash_offset, cur_alloc,
                                         align_up_4k(new_capacity + 1))) {
                /* Expand in-place */
                uint32_t extra_alloc = align_up_4k(new_capacity + 1) - cur_alloc;
                if (extra_alloc > 0) {
                    flash_erase(fs, f->flash_offset + cur_alloc, extra_alloc);
                }

                int new_slot = mmrofs_create_entry(fs, f->name_len, f->name_hash,
                                                    f->flash_offset, new_capacity,
                                                    cur.ctime, f->entry_index, ENTRY_NONE);
                if (new_slot < 0) {
                    mmrofs_unlock(fs);
                    errno = ENOSPC;
                    return -1;
                }

                mmrofs_tombstone_old_flow(fs, (uint16_t)new_slot, f->entry_index, ENTRY_NONE);

                /* Write the new data */
                flash_write(fs, f->flash_offset + f->name_len + f->data_size, data, size);

                f->entry_index = (uint16_t)new_slot;
                f->data_size = new_data_total;

            } else {
                /* Full rewrite to new location */
                uint32_t new_alloc = align_up_4k(new_capacity + 1);
                uint32_t new_offset;
                if (mmrofs_find_free_data(fs, new_alloc, &new_offset) < 0) {
                    mmrofs_unlock(fs);
                    errno = ENOSPC;
                    return -1;
                }

                flash_erase(fs, new_offset, new_alloc);

                int new_slot = mmrofs_create_entry(fs, f->name_len, f->name_hash,
                                                    new_offset, new_capacity,
                                                    cur.ctime, f->entry_index, ENTRY_NONE);
                if (new_slot < 0) {
                    mmrofs_unlock(fs);
                    errno = ENOSPC;
                    return -1;
                }

                /* Copy filename + existing data */
                flash_write(fs, new_offset, f->filename, f->name_len);

                uint8_t copy_buf[256];
                uint32_t src_pos = f->flash_offset + f->name_len;
                uint32_t dst_pos = new_offset + f->name_len;
                uint32_t remaining = f->data_size;
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(copy_buf) ? sizeof(copy_buf) : remaining;
                    mmrofs_data_read(fs, src_pos, copy_buf, chunk);
                    flash_write(fs, dst_pos, copy_buf, chunk);
                    src_pos += chunk;
                    dst_pos += chunk;
                    remaining -= chunk;
                }

                mmrofs_tombstone_old_flow(fs, (uint16_t)new_slot, f->entry_index, ENTRY_NONE);

                /* Write new data */
                flash_write(fs, new_offset + f->name_len + f->data_size, data, size);

                f->entry_index = (uint16_t)new_slot;
                f->flash_offset = new_offset;
                f->data_size = new_data_total;
            }
        }

        mmrofs_unlock(fs);
        return (ssize_t)size;
    }

    mmrofs_unlock(fs);
    errno = EBADF;
    return -1;
}

/* ---- read ---- */

static ssize_t vfs_read(void *ctx, int fd_num, void *dst, size_t size)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    mmrofs_fd_t *f = mmrofs_get_fd(fs, fd_num);
    if (!f) { errno = EBADF; return -1; }

    if (f->fd_state != FD_STATE_COMMITTED) {
        /* Not yet written — no data to read */
        return 0;
    }

    if (f->data_offset >= f->data_size)
        return 0; /* EOF */

    size_t avail = f->data_size - f->data_offset;
    if (size > avail)
        size = avail;

    uint32_t read_pos = f->flash_offset + f->name_len + f->data_offset;

    esp_err_t ret = mmrofs_data_read(fs, read_pos, dst, size);
    if (ret != ESP_OK) {
        errno = EIO;
        return -1;
    }

    f->data_offset += size;
    return (ssize_t)size;
}

/* ---- close ---- */

static int vfs_close(void *ctx, int fd_num)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    mmrofs_fd_t *f = mmrofs_get_fd(fs, fd_num);
    if (!f) { errno = EBADF; return -1; }

    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        /* Read-only: just free the FD */
        mmrofs_free_fd(fs, fd_num);
        return 0;
    }

    mmrofs_lock(fs);

    if (f->fd_state == FD_STATE_COMMITTED) {
        /* Write mtime (field starts as 0xFFFFFFFF, always safe 1→0) */
        uint32_t now = (uint32_t)time(NULL);
        entry_write_mtime(fs, f->entry_index, now);

        /* Finalize exact size (entry holds capacity mask with all low bits 1,
         * so writing the smaller exact_size is always a safe 1→0 transition) */
        uint32_t exact_size = (uint32_t)f->name_len + f->data_size;
        entry_write_size(fs, f->entry_index, exact_size);

        /* ACTIVE → VALID */
        entry_write_state(fs, f->entry_index, STATE_VALID);
    }
    /* If PENDING_NEW or PENDING_UPDATE and never written: no entry to finalize */

    mmrofs_free_fd(fs, fd_num);
    mmrofs_unlock(fs);
    return 0;
}

/* ---- fstat ---- */

static int vfs_fstat(void *ctx, int fd_num, struct stat *st)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    mmrofs_fd_t *f = mmrofs_get_fd(fs, fd_num);
    if (!f) { errno = EBADF; return -1; }

    memset(st, 0, sizeof(*st));

    mmrofs_lock(fs);

    if (f->fd_state == FD_STATE_COMMITTED && f->entry_index != 0xFFFF) {
        mmrofs_entry_t e;
        if (entry_read(fs, f->entry_index, &e) == ESP_OK) {
            st->st_size = e.size - e.name_len;
            st->st_mode = S_IFREG | 0444;
            st->st_mtime = e.mtime == MTIME_UNSET ? 0 : e.mtime;
            st->st_ctime = e.ctime;
        }
    } else {
        /* Pending — return what we have from the FD */
        st->st_size = f->data_size;
        st->st_mode = S_IFREG | 0444;
    }

    mmrofs_unlock(fs);
    return 0;
}

/* ---- stat ---- */

static int vfs_stat(void *ctx, const char *path, struct stat *st)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;

    if (path[0] == '/') path++;
    uint16_t name_len = (uint16_t)strlen(path);
    if (name_len == 0 || name_len > MMROFS_MAX_FILENAME_LEN) {
        errno = EINVAL;
        return -1;
    }

    uint32_t hash = fnv1a32(path, name_len);

    mmrofs_lock(fs);

    mmrofs_entry_t e;
    int idx = mmrofs_lookup(fs, path, name_len, hash, &e);
    if (idx < 0) {
        mmrofs_unlock(fs);
        errno = ENOENT;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_size = e.size - e.name_len;
    st->st_mode = S_IFREG | 0444;
    st->st_mtime = e.mtime == MTIME_UNSET ? 0 : e.mtime;
    st->st_ctime = e.ctime;

    mmrofs_unlock(fs);
    return 0;
}

/* ---- lseek ---- */

static off_t vfs_lseek(void *ctx, int fd_num, off_t offset, int whence)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    mmrofs_fd_t *f = mmrofs_get_fd(fs, fd_num);
    if (!f) { errno = EBADF; return -1; }

    /* lseek not supported on write-only FDs */
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        errno = ESPIPE;
        return -1;
    }

    off_t new_pos;
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = (off_t)f->data_offset + offset;
        break;
    case SEEK_END:
        new_pos = (off_t)f->data_size + offset;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    if (new_pos < 0 || (uint32_t)new_pos > f->data_size) {
        errno = EINVAL;
        return -1;
    }

    f->data_offset = (uint32_t)new_pos;
    return new_pos;
}

/* ---- unlink ---- */

static int vfs_unlink(void *ctx, const char *path)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;

    if (path[0] == '/') path++;
    uint16_t name_len = (uint16_t)strlen(path);
    if (name_len == 0 || name_len > MMROFS_MAX_FILENAME_LEN) {
        errno = EINVAL;
        return -1;
    }

    uint32_t hash = fnv1a32(path, name_len);

    mmrofs_lock(fs);

    int idx = mmrofs_lookup(fs, path, name_len, hash, NULL);
    if (idx < 0) {
        mmrofs_unlock(fs);
        errno = ENOENT;
        return -1;
    }

    entry_write_state(fs, (uint16_t)idx, STATE_TOMBSTONE);

    mmrofs_unlock(fs);
    return 0;
}

/* ---- rename ---- */

static int vfs_rename(void *ctx, const char *src_path, const char *dst_path)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;

    if (src_path[0] == '/') src_path++;
    if (dst_path[0] == '/') dst_path++;

    uint16_t src_name_len = (uint16_t)strlen(src_path);
    uint16_t dst_name_len = (uint16_t)strlen(dst_path);

    if (src_name_len == 0 || src_name_len > MMROFS_MAX_FILENAME_LEN ||
        dst_name_len == 0 || dst_name_len > MMROFS_MAX_FILENAME_LEN) {
        errno = EINVAL;
        return -1;
    }

    uint32_t src_hash = fnv1a32(src_path, src_name_len);
    uint32_t dst_hash = fnv1a32(dst_path, dst_name_len);

    mmrofs_lock(fs);

    /* Lookup src */
    mmrofs_entry_t src_entry;
    int src_idx = mmrofs_lookup(fs, src_path, src_name_len, src_hash, &src_entry);
    if (src_idx < 0) {
        mmrofs_unlock(fs);
        errno = ENOENT;
        return -1;
    }

    /* Lookup dst (may or may not exist) */
    int dst_idx = mmrofs_lookup(fs, dst_path, dst_name_len, dst_hash, NULL);

    uint32_t src_data_size = src_entry.size - src_entry.name_len;
    uint32_t new_total = dst_name_len + src_data_size;
    uint32_t new_alloc = align_up_4k(new_total);

    /* Allocate new data region */
    uint32_t new_offset;
    if (mmrofs_find_free_data(fs, new_alloc, &new_offset) < 0) {
        mmrofs_unlock(fs);
        errno = ENOSPC;
        return -1;
    }

    flash_erase(fs, new_offset, new_alloc);

    /* Create new entry */
    uint32_t dst_entry_val = (dst_idx >= 0) ? (uint32_t)dst_idx : ENTRY_NONE;
    int new_slot = mmrofs_create_entry(fs, dst_name_len, dst_hash,
                                        new_offset, new_total,
                                        src_entry.ctime, (uint32_t)src_idx, dst_entry_val);
    if (new_slot < 0) {
        mmrofs_unlock(fs);
        errno = ENOSPC;
        return -1;
    }

    /* Write new filename */
    flash_write(fs, new_offset, dst_path, dst_name_len);

    /* Copy file data from src */
    if (src_data_size > 0) {
        uint8_t copy_buf[256];
        uint32_t src_pos = src_entry.offset + src_entry.name_len;
        uint32_t dst_pos = new_offset + dst_name_len;
        uint32_t remaining = src_data_size;

        while (remaining > 0) {
            size_t chunk = remaining > sizeof(copy_buf) ? sizeof(copy_buf) : remaining;
            mmrofs_data_read(fs, src_pos, copy_buf, chunk);
            flash_write(fs, dst_pos, copy_buf, chunk);
            src_pos += chunk;
            dst_pos += chunk;
            remaining -= chunk;
        }
    }

    /* TOMBSTONING_OLD flow */
    entry_write_state(fs, (uint16_t)new_slot, STATE_TOMBSTONING_OLD);
    entry_write_state(fs, (uint16_t)src_idx, STATE_TOMBSTONE);

    if (dst_idx >= 0) {
        entry_write_state(fs, (uint16_t)dst_idx, STATE_TOMBSTONE);
    }

    /* Write mtime */
    uint32_t now = (uint32_t)time(NULL);
    entry_write_mtime(fs, (uint16_t)new_slot, now);

    /* TOMBSTONING_OLD → ACTIVE → VALID */
    entry_write_state(fs, (uint16_t)new_slot, STATE_ACTIVE);
    entry_write_state(fs, (uint16_t)new_slot, STATE_VALID);

    mmrofs_unlock(fs);
    return 0;
}

/* ---- opendir / readdir / closedir ---- */

static DIR *vfs_opendir(void *ctx, const char *path)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    (void)path; /* flat namespace — path is ignored */

    mmrofs_lock(fs);

    for (int i = 0; i < MMROFS_MAX_DIRS; i++) {
        if (!fs->dirs[i].in_use) {
            fs->dirs[i].in_use = true;
            fs->dirs[i].scan_index = 0;
            mmrofs_unlock(fs);
            return (DIR *)&fs->dirs[i];
        }
    }

    mmrofs_unlock(fs);
    errno = ENOMEM;
    return NULL;
}

static struct dirent *vfs_readdir(void *ctx, DIR *pdir)
{
    mmrofs_t *fs = (mmrofs_t *)ctx;
    mmrofs_dir_t *dir = (mmrofs_dir_t *)pdir;

    static struct dirent de;
    mmrofs_entry_t e;

    mmrofs_lock(fs);

    while (dir->scan_index < fs->max_entries) {
        uint16_t idx = dir->scan_index++;
        if (entry_read(fs, idx, &e) != ESP_OK)
            continue;
        if (!entry_is_live(e.state))
            continue;
        if (!entry_validate(fs, &e))
            continue;

        /* Read filename */
        uint16_t read_len = e.name_len;
        if (read_len > sizeof(de.d_name) - 1)
            read_len = sizeof(de.d_name) - 1;

        if (mmrofs_data_read(fs, e.offset, de.d_name, read_len) != ESP_OK)
            continue;

        de.d_name[read_len] = '\0';
        de.d_ino = idx;
        de.d_type = DT_REG;

        mmrofs_unlock(fs);
        return &de;
    }

    mmrofs_unlock(fs);
    return NULL;
}

static int vfs_closedir(void *ctx, DIR *pdir)
{
    (void)ctx;
    mmrofs_dir_t *dir = (mmrofs_dir_t *)pdir;
    dir->in_use = false;
    return 0;
}

/* --------------------------------------------------------------------------
 * Registration: mmrofs_register_vfs
 * -------------------------------------------------------------------------- */

esp_err_t mmrofs_register_vfs(const mmrofs_mount_cfg_t *cfg)
{
    if (!cfg || !cfg->base_path || !cfg->partition_label || cfg->max_files <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mmrofs) {
        ESP_LOGE(TAG, "MMROFS already mounted");
        return ESP_ERR_INVALID_STATE;
    }

    /* Find partition */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        cfg->partition_label);

    if (!part) {
        ESP_LOGE(TAG, "Partition '%s' not found", cfg->partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Partition '%s': addr=0x%lx size=0x%lx (%lu bytes)",
             part->label, (unsigned long)part->address,
             (unsigned long)part->size, (unsigned long)part->size);

    if (part->size < MMROFS_HEADER_SIZE + MMROFS_ERASE_BLOCK_SIZE) {
        ESP_LOGE(TAG, "Partition too small for MMROFS");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Allocate FS context */
    mmrofs_t *fs = calloc(1, sizeof(mmrofs_t));
    if (!fs) return ESP_ERR_NO_MEM;

    fs->partition = part;
    fs->partition_size = part->size;
    fs->max_files = cfg->max_files;
    fs->max_entries = (MMROFS_HEADER_SIZE) / MMROFS_ENTRY_SIZE;
    fs->next_free_entry = 0;
    fs->mmap_valid = false;

    /* Allocate FD table */
    fs->fds = calloc(cfg->max_files, sizeof(mmrofs_fd_t));
    if (!fs->fds) {
        free(fs);
        return ESP_ERR_NO_MEM;
    }

    /* Create mutex */
    fs->mutex = xSemaphoreCreateRecursiveMutex();
    if (!fs->mutex) {
        free(fs->fds);
        free(fs);
        return ESP_ERR_NO_MEM;
    }

    /* Mmap full 64 KB header region (permanent, used for all entry reads) */
    esp_err_t mmap_ret = esp_partition_mmap(part, 0, MMROFS_HEADER_SIZE,
                                             ESP_PARTITION_MMAP_DATA,
                                             &fs->header_mmap_ptr,
                                             &fs->header_mmap_handle);
    if (mmap_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap header region: %s", esp_err_to_name(mmap_ret));
        vSemaphoreDelete(fs->mutex);
        free(fs->fds);
        free(fs);
        return mmap_ret;
    }

    /* Initialize clock from filesystem if system time is invalid */
    mmrofs_init_clock(fs);

    /* Run recovery */
    mmrofs_recover(fs);

    /* Register with VFS */
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = vfs_open,
        .write_p = vfs_write,
        .read_p = vfs_read,
        .close_p = vfs_close,
        .fstat_p = vfs_fstat,
        .stat_p = vfs_stat,
        .lseek_p = vfs_lseek,
        .unlink_p = vfs_unlink,
        .rename_p = vfs_rename,
        .opendir_p = vfs_opendir,
        .readdir_p = vfs_readdir,
        .closedir_p = vfs_closedir,
    };

    esp_err_t ret = esp_vfs_register(cfg->base_path, &vfs, fs);
    if (ret != ESP_OK) {
        vSemaphoreDelete(fs->mutex);
        free(fs->fds);
        free(fs);
        return ret;
    }

    s_mmrofs = fs;
    ESP_LOGI(TAG, "Mounted at '%s' (max_entries=%u, max_files=%d)",
             cfg->base_path, fs->max_entries, fs->max_files);

    return ESP_OK;
}
