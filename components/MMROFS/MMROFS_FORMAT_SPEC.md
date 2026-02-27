# MMROFS (Memory-Mapped Read-Only-ish File System) Format Specification v2

## Overview

MMROFS is a transactional, crash-safe filesystem designed for embedded systems with limited flash storage. It provides deterministic recovery after power loss without checksums or generation numbers, using only state machine transitions.

**Key Properties:**
- Transactional writes with state machine transitions (single-byte flash writes)
- Direct SPI flash writes (no RAM buffering required)
- Single 64 KB sliding mmap window for data reads
- FNV-1a 32-bit hash for fast file lookup
- All multi-byte fields are **little-endian**
- Mutex-based concurrency control

---

## Partition Layout

```
┌─────────────────────────────────┐
│  Header (64 KB)                 │  Offset: 0x00000 - 0x0FFFF
│  - Entry table (60 KB usable)   │
│  - Reserved for future metadata │
└─────────────────────────────────┘
│  Data Region                    │  Offset: 0x10000 - end
│  - File data (variable sizes)   │
│  - Erased regions (0xFF bytes)  │
└─────────────────────────────────┘
```

### Header Region (64 KB)

**Erase Block:** 4 KB (minimum erase granularity)

The entry table starts at offset 0 within the header. Entries are packed sequentially in 32-byte slots. A persistent **next_free_entry** index is tracked in RAM (rebuilt on mount) to avoid scanning from the beginning on every allocation.

---

## Entry Structure (32 bytes, little-endian)

```c
struct mmrofs_entry_t {
    uint8_t  state;           // Offset 0     - Entry state (1 byte)
    uint8_t  reserved_v;      // Offset 1     - Reserved (1 byte, keep 0x00)
    uint16_t name_len;        // Offset 2-3   - Filename length 1-65535 (2 bytes, LE)
    uint32_t name_hash;       // Offset 4-7   - FNV-1a 32-bit hash (4 bytes, LE)
    uint32_t offset;          // Offset 8-11  - Partition-relative data offset (4 bytes, LE)
    uint32_t size;            // Offset 12-15 - Total on-flash size: name_len + data (4 bytes, LE)
    uint32_t mtime;           // Offset 16-19 - Modification time (4 bytes, LE)
    uint32_t ctime;           // Offset 20-23 - Creation time (4 bytes, LE)
    uint32_t old_entry;       // Offset 24-27 - Old entry index for TOMBSTONING_OLD (4 bytes, LE)
    uint32_t dst_entry;       // Offset 28-31 - Dst entry index to tombstone on rename (4 bytes, LE)
};
```

### Entry Fields

| Field | Size | Description |
|-------|------|-------------|
| `state` | 1B | Current lifecycle state (see State Machine) |
| `reserved_v` | 1B | Reserved, must be 0x00; ignore on read |
| `name_len` | 2B | Length of filename in bytes (1–65535); zero is invalid |
| `name_hash` | 4B | FNV-1a 32-bit hash of filename |
| `offset` | 4B | Partition-relative byte offset to start of file data region; must be ≥ 0x10000 |
| `size` | 4B | Total on-flash size (filename + file data). Bottom 12 bits are a capacity mask during streaming writes — all 1s until finalized by `close()`. Data-only size = `size - name_len`. See **Streaming Allocation** |
| `mtime` | 4B | Modification time (Unix timestamp, seconds since epoch). Left as 0xFFFFFFFF during allocation; written at `close()` or finalization before VALID |
| `ctime` | 4B | Creation/change time (Unix timestamp, seconds since epoch) |
| `old_entry` | 4B | Slot index of previous entry for TOMBSTONING_OLD recovery; 0xFFFFFFFF for new files |
| `dst_entry` | 4B | Slot index of existing destination entry to tombstone on rename; 0xFFFFFFFF if not a rename-over-existing |

### Total On-Flash Size Per File

```
data_size = entry.size - entry.name_len
allocated_bytes = ceil(entry.size / 4096) * 4096
allocated_blocks = allocated_bytes / 4096
```

### Streaming Allocation (12-Bit Capacity Mask)

When the total file size is **not known** at write time, the `size` field uses a 12-bit capacity mask scheme that exploits SPI NOR flash's "can only clear bits" property.

**Encoding:** The bottom 12 bits of `size` are initially written as all 1s (0xFFF), reserving them for later finalization. The upper 20 bits encode the block-count prefix.

| `size` value | Allocated bytes | Meaning |
|-------------|-----------------|--------|
| 0x00000FFF | 4,096 (1 block) | Streaming, up to 4 KB total (filename + data) |
| 0x00001FFF | 8,192 (2 blocks) | Streaming, up to 8 KB total |
| 0x00002FFF | 12,288 (3 blocks) | Streaming, up to 12 KB total |
| 0x0000NFFF | (N+1) × 4,096 | Streaming, up to (N+1) × 4 KB total |

**On first write** (size unknown): entry is allocated with `size = 0x00000FFF` (1 block). The entry transitions to ACTIVE immediately.

**Capacity expansion:** If data exceeds the current allocation:
1. Verify the next 4 KB block (at `entry.offset + allocated_bytes`) is erased or reclaimable.
2. Allocate a new entry with the next capacity tier (e.g., `0x00001FFF`), same `offset`.
3. Transition via TOMBSTONING_OLD: tombstone the old entry, mark new entry ACTIVE.
4. Continue writing.
5. If the next block is not available: fall back to full rewrite at a new offset with sufficient capacity.

**On close:** Write the exact total size (`name_len + data_bytes_written`) to the `size` field. Since the exact value is always ≤ the capacity mask value, this only clears bits — flash-safe. Example: `0x00000FFF` → `0x00000013` (19 bytes: 8-byte filename + 11 bytes data).

**Known-size writes:** When the total size is known upfront, `size` is written with the exact value directly (e.g., `size = name_len + data_size`). No capacity mask is used.

**Detection:** An `ACTIVE` state indicates the file has not been closed yet. Additionally, `(entry.size & 0xFFF) == 0xFFF` indicates the size is still a capacity mask (unfinalised). The file data is valid but the exact length is unknown — readers see up to `entry.size - entry.name_len` bytes, which may include trailing erased (0xFF) bytes at the end of the last block.

---

## State Machine

Each state transition is a single-byte write to the `state` field (byte 0 of the entry). Because SPI NOR flash can only clear bits (1→0), each transition strictly clears additional bits, making every transition safe without an erase cycle.

### State Values

| State | Value | Binary | Meaning |
|-------|-------|--------|---------|
| FREE | 0xFF | 11111111 | Slot never written (erased flash) |
| ALLOCATING | 0x7F | 01111111 | Slot reserved; metadata being written |
| PENDING_DATA | 0x3F | 00111111 | Metadata committed; data being written |
| TOMBSTONING_OLD | 0x1F | 00011111 | New entry ready; old entry needs tombstone |
| ACTIVE | 0x0F | 00001111 | Entry is readable; size and mtime may not be finalized |
| VALID | 0x07 | 00000111 | Entry is fully committed; size and mtime are exact |
| TOMBSTONE | 0x03 | 00000011 | Entry deleted; data reclaimable |
| BADBLOCK | 0x01 | 00000001 | Flash block unreliable; skip permanently |
| ERASED | 0x00 | 00000000 | Slot was corrupted or cleared; skip |

### Ordering of Writes Within a State Transition

The `state` byte is the **commit point** for each phase:

1. **FREE → ALLOCATING:** Write `state = 0x7F` as the **first byte** before writing any other metadata fields. This reserves the slot.
2. **Write metadata:** After ALLOCATING is committed, write `name_len`, `name_hash`, `offset`, `size`, `ctime`, `old_entry`, and `dst_entry` to the remaining entry fields. Leave `mtime` as 0xFFFFFFFF (erased) — it is written at `close()` or finalization time. Set `dst_entry = 0xFFFFFFFF` for non-rename operations; for renames over an existing destination, set `dst_entry` to the dst slot index.
3. **ALLOCATING → PENDING_DATA:** Write `state = 0x3F` only after **all metadata fields** are committed to flash.
4. **Write filename:** Only after PENDING_DATA is committed, write the filename to the data region at `entry.offset`.
5. **Write file data:** After the filename, write the actual file data bytes at `entry.offset + entry.name_len`.
6. **PENDING_DATA → ACTIVE** (new file) or **PENDING_DATA → TOMBSTONING_OLD → TOMBSTONE old → ACTIVE** (update/rename): Entry becomes ACTIVE during the first `write()`. The file is readable but the `size` field may be a capacity mask.
7. **ACTIVE → VALID:** Written by `close()` after writing `mtime` and finalizing the exact `size`. Both are 4-byte writes that can be torn by power loss. ACTIVE recovery detects and corrects torn writes before promoting to VALID. VALID means the entry is fully committed — `size` and `mtime` are exact.

This ordering guarantees:
- A crash during metadata write → state is ALLOCATING → recovery tombstones it
- A crash during data write → state is PENDING_DATA → recovery tombstones it
- A crash after ACTIVE → file is readable; recovery always validates size by scanning backwards for trailing 0xFF (handles both capacity-mask and torn size writes). If mtime was never written (0xFFFFFFFF), write it and promote to VALID. If mtime was partially written (torn), allocate a new entry with correct mtime via TOMBSTONING_OLD.
- A crash after VALID → file is fully committed, no recovery needed

### State Transition Flows

#### New File Write
```
FREE
  ↓ write state = 0x7F
ALLOCATING
  ↓ write name_len, name_hash, offset, size, ctime, old_entry, dst_entry (mtime left as 0xFFFFFFFF)
  ↓   (size = name_len + data_size if known, or capacity mask e.g. 0xFFF for streaming)
  ↓ write state = 0x3F
PENDING_DATA
  ↓ write filename to data region at entry.offset
  ↓ write file data to data region at entry.offset + name_len
  ↓ write state = 0x0F
ACTIVE
  ↓ [subsequent writes extend data in place]
  ↓ [capacity expansion via TOMBSTONING_OLD if block boundary exceeded]
  ↓ close(): write mtime, finalize size (clear capacity mask to exact value)
  ↓ write state = 0x07
VALID
```

#### Append (space ahead is erased or tombstoned)
```
[old entry: VALID (size exact), or ACTIVE with known data size from FD]

1. Allocate NEW entry slot:
   FREE → ALLOCATING (write state = 0x7F)
2. Write metadata: same offset as old, larger size (name_len + old_data + new_data),
   preserve ctime, set old_entry (mtime left as 0xFFFFFFFF)
   ALLOCATING → PENDING_DATA (write state = 0x3F)
3. New entry points to same offset; old filename is reused in the data region.
   Write ONLY the new appended data at: old_offset + name_len + old_data_size
4. PENDING_DATA → TOMBSTONING_OLD (write state = 0x1F)
5. Write old entry state = TOMBSTONE (0x03)
6. Write new entry state = ACTIVE (0x0F)
```

**Append preconditions:**
- **The old entry's exact data size must be known.** This is satisfied when:
  - The old entry is VALID (size is exact: `old_data_size = old_entry.size - old_entry.name_len`), or
  - The old entry is ACTIVE but `old_data_size` is known from the FD that wrote it (e.g., same FD doing a capacity expansion via TOMBSTONING_OLD)
- If the old entry is ACTIVE with an unfinalised capacity-mask size (`(old_entry.size & 0xFFF) == 0xFFF`) and no FD is tracking the actual data size: **append is not possible**. Fall through to error or recovery-first (see below).
- The flash region immediately after the old file's allocated blocks must be either erased (0xFF) or covered by TOMBSTONE entries
- The filename in the old data region remains valid and is reused
- The new entry's `size` field reflects the total on-flash size (name_len + old data + appended data), computed from the known exact `old_data_size`
- The new entry's `mtime` is left as 0xFFFFFFFF — written at `close()` time
- The new entry's `ctime` is copied from the old entry's `ctime`
- The new entry's `old_entry` is set to the old entry's slot index

**Append fallback:** If space ahead is not available (occupied by another VALID or ACTIVE file), fall through to Full Rewrite. If the old entry's exact data size is unknown (ACTIVE with capacity mask and no FD tracking), return `EIO` — the file must be recovered first (e.g., by closing the other FD or rebooting, which triggers ACTIVE recovery).

#### Full Rewrite (Replace Existing)
```
[old entry: VALID (size exact), or ACTIVE with known data size from FD]

1. Allocate NEW entry slot at new data offset:
   FREE → ALLOCATING (write state = 0x7F)
2. Write metadata (new offset, size = name_len + data_size, preserve ctime, set old_entry; mtime left as 0xFFFFFFFF):
   ALLOCATING → PENDING_DATA (write state = 0x3F)
3. Write filename + data to new data region
4. PENDING_DATA → TOMBSTONING_OLD (write state = 0x1F)
5. Write old entry state = TOMBSTONE (0x03)
6. Write new entry state = ACTIVE (0x0F)
```

#### File Delete
```
VALID or ACTIVE
  ↓ write state = 0x03
TOMBSTONE
```

#### Rename (Atomic via TOMBSTONING_OLD)
```
[old entry: VALID, filename="old.txt"]
[dst entry: VALID, filename="new.txt" (if exists)]

1. Allocate NEW entry slot:
   FREE → ALLOCATING (write state = 0x7F)
2. Write metadata: new name_hash, new name_len, new offset,
   size = new_name_len + (src.size - src.name_len), preserve ctime, set old_entry to src index,
   set dst_entry to dst index (or 0xFFFFFFFF if dst does not exist)
   (mtime left as 0xFFFFFFFF)
   ALLOCATING → PENDING_DATA (write state = 0x3F)
3. Write new filename to new data offset, then copy file data from
   old location to new location
   (since filename length may differ, the entire file must be rewritten)
4. PENDING_DATA → TOMBSTONING_OLD (write state = 0x1F)
5. Write old (src) entry state = TOMBSTONE (0x03)
6. If dst_entry != 0xFFFFFFFF: write dst entry state = TOMBSTONE (0x03)
7. Write mtime to new entry
8. Write new entry state = ACTIVE (0x0F)
9. Write new entry state = VALID (0x07)
```

**Rename atomicity:** The destination entry is **not** tombstoned before the new entry reaches TOMBSTONING_OLD. This guarantees that if power is lost before TOMBSTONING_OLD, the destination file is untouched and the rename is cleanly rolled back. Both `old_entry` (src) and `dst_entry` (dst) are recorded in the entry metadata during ALLOCATING, so recovery can complete both tombstones atomically.

**Note:** If `name_len` is identical for old and new names, an optimization could reuse the same data offset by overwriting only the filename bytes. However, since flash can only clear bits, this is only safe if every byte of the new filename has the same or fewer set bits than the old — which is generally not guaranteed. Therefore, rename always performs a full copy to a new data region.

### Recovery Rules (Boot-Time)

On mount, `mmrofs_recover()` scans **all** entry slots (0 through `max_entries - 1`) and applies:

| State | Recovery Action |
|-------|-----------------|
| FREE (0xFF) | Check if entire 32-byte slot is 0xFF. If not, write state = ERASED (0x00) and skip. If yes, skip (unused slot). |
| ALLOCATING (0x7F) | Write state = TOMBSTONE (0x03). Incomplete allocation. |
| PENDING_DATA (0x3F) | Write state = TOMBSTONE (0x03). Data write was interrupted. |
| TOMBSTONING_OLD (0x1F) | **Validate entry** (see Entry Validation). If invalid: tombstone and skip. Read `old_entry` and `dst_entry` fields from the entry struct. Validate that `old_entry` references a valid slot in VALID or ACTIVE state; if so, mark it TOMBSTONE (0x03). If `dst_entry` is not 0xFFFFFFFF and references a valid VALID/ACTIVE slot, mark it TOMBSTONE (0x03). Mark this entry as ACTIVE (0x0F). |
| ACTIVE (0x0F) | **Validate entry** (see Entry Validation). If invalid: tombstone and skip. **Size recovery** (always performed, handles both capacity-mask and torn writes): Scan data backwards from `offset + allocated_bytes` to find last non-0xFF byte; compute `inferred_size = (last_non_FF_offset - offset) + 1`. If `(entry.size & 0xFFF) == 0xFFF` (capacity mask): write `inferred_size` as exact size. Else if current `size` differs from `inferred_size` **and** the file has more than 2 trailing 0xFF bytes (comparing data at `offset + size - 1`, `offset + size - 2`, `offset + size - 3`): size was likely torn — write `inferred_size`. Else: size appears valid, leave as-is. **Mtime recovery:** If `entry.mtime == 0xFFFFFFFF`: write current system time (never programmed). If `entry.mtime != 0xFFFFFFFF`: could be valid or torn — cannot repair in-place. Allocate a new entry via TOMBSTONING_OLD with all fields copied, mtime = current system time, tombstone this entry, promote new entry to VALID (0x07). If `entry.mtime == 0xFFFFFFFF` (simple case): write mtime, then write state = VALID (0x07). |
| VALID (0x07) | **Validate entry** (see Entry Validation). If invalid: tombstone and skip. Otherwise leave as-is. Fully committed entry. |
| TOMBSTONE (0x03) | Leave as-is. |
| BADBLOCK (0x01) | Leave as-is. Skip permanently. |
| ERASED (0x00) | Leave as-is. Skip permanently. |

**Additional recovery:** During the full scan, rebuild the `next_free_entry` index by finding the lowest-indexed FREE slot (where all 32 bytes are 0xFF).

**TOMBSTONING_OLD recovery** uses the `old_entry` and `dst_entry` fields stored directly in the entry struct. During any operation that creates a TOMBSTONING_OLD state (update, append, rename), the old entry's slot index is written into `old_entry` and any existing destination's slot index is written into `dst_entry` as part of the entry metadata during the ALLOCATING phase — before the transition to PENDING_DATA. This guarantees:

1. Recovery reads `old_entry` (uint32_t LE at offset 24) and `dst_entry` (uint32_t LE at offset 28) from the TOMBSTONING_OLD entry struct.
2. If `old_entry` is a valid slot index and that slot's state is VALID or ACTIVE: mark it TOMBSTONE (0x03).
3. If `dst_entry` is a valid slot index (not 0xFFFFFFFF) and that slot's state is VALID or ACTIVE: mark it TOMBSTONE (0x03).
4. Mark the TOMBSTONING_OLD entry as ACTIVE (0x0F). ACTIVE recovery (see above) will then finalize size and mtime if needed, and promote to VALID.
5. If either field is 0xFFFFFFFF or the referenced slot is not VALID/ACTIVE (e.g., already tombstoned by a prior recovery): skip that tombstone. No error — the entry was already cleaned up.

This approach is deterministic and handles all cases (update, append, rename) uniformly. Since `old_entry` is in the entry struct itself, recovery requires no data region reads.

### Torn Write Handling (ACTIVE Recovery Detail)

SPI NOR flash programs 4 bytes at a time. A power loss during a 4-byte write can leave a partially programmed value — neither the original erased value nor the intended value. ACTIVE recovery handles this for both `size` and `mtime`:

#### Size Recovery Algorithm

```
allocated_bytes = ceil(entry.size / 4096) * 4096
scan backwards from (entry.offset + allocated_bytes - 1) to entry.offset + entry.name_len
find last_non_FF_offset (last byte that is not 0xFF)
inferred_size = (last_non_FF_offset - entry.offset) + 1

if (entry.size & 0xFFF) == 0xFFF:
    // Capacity mask — file was never closed properly
    write inferred_size to entry.size

else if inferred_size < entry.size:
    // Check for suspicious trailing 0xFF
    trailing_FF_count = entry.size - inferred_size
    if trailing_FF_count > 2:
        // Size was likely torn — more than 2 trailing 0xFF is implausible
        write inferred_size to entry.size
    else:
        // 0-2 trailing 0xFF bytes — plausible file content, accept size as-is
        (no change)

else:
    // inferred_size >= entry.size — data fills to written size, accept as-is
    (no change)
```

**Trade-off:** Files whose legitimate content ends with 3+ consecutive 0xFF bytes will have their size trimmed on ACTIVE recovery. This is an accepted heuristic for an embedded filesystem where such content is rare.

#### Mtime Recovery Algorithm

```
if entry.mtime == 0xFFFFFFFF:
    // Never programmed — write current system time in-place (clears bits, flash-safe)
    write current_system_time to entry.mtime
    write state = VALID (0x07)

else:
    // Could be valid OR torn — cannot distinguish, cannot repair in-place
    // (flash can only clear bits; a torn value may have bits cleared that
    //  the correct time needs set)
    // Fix: allocate a new entry and tombstone this one
    1. Allocate new entry slot: FREE → ALLOCATING (0x7F)
    2. Copy all metadata fields from this entry (name_len, name_hash, offset,
       size [as corrected above], ctime, old_entry=this_slot_index,
       dst_entry=0xFFFFFFFF)
    3. Write mtime = current system time
    4. ALLOCATING → PENDING_DATA (0x3F)
    5. (No data write needed — data region is shared with old entry)
    6. PENDING_DATA → TOMBSTONING_OLD (0x1F)
    7. Write this (old) entry state = TOMBSTONE (0x03)
    8. Write new entry state = ACTIVE (0x0F)
    9. Recurse: apply ACTIVE recovery to the new entry
       (mtime is now 0xFFFFFFFF? No — we just wrote it. So mtime != 0xFFFFFFFF.
        But we KNOW it's valid because we just wrote it successfully.
        Check: if new entry.mtime != 0xFFFFFFFF, this time we wrote it ourselves
        during this same recovery pass — so trust it and promote to VALID.)
   10. Write new entry state = VALID (0x07)
```

**Note:** The re-entry case (step 9) is safe because the mtime was written in the same boot session. If power dies during this recovery operation itself, TOMBSTONING_OLD recovery will restart the process on the next boot.

---

## Data Region

**Starts at:** Offset 0x10000 (64 KB from partition start)
**Layout:** Free-form, first-fit allocation
**Alignment:** 4096-byte aligned

### File Data Layout

Each file's data region contains the **filename** followed by the **file data**. All per-file metadata (mtime, ctime, old_entry) is stored in the entry struct, not in the data region.

```
Offset (relative to entry.offset):
  [0x00 - 0x00+L-1] filename (L bytes, where L = entry.name_len, NOT null-terminated)
  [L - L+D-1]       file data (D bytes, where D = entry.size - entry.name_len)
```

**Total on-flash per file:** `entry.size` bytes (includes filename)

---

## Time Management

### Boot-Time Clock Initialization

When the filesystem is mounted, before any write operations are permitted:

1. Read the current system time.
2. If the system time appears invalid (year < 1990):
   a. Scan all VALID and ACTIVE entries in the entry table.
   b. For each VALID or ACTIVE entry, read its `mtime` and `ctime` from the entry (skip entries where `mtime == 0xFFFFFFFF`).
   c. Find the **maximum** of all `mtime` and `ctime` values across all files.
   d. If a valid maximum is found (> 0 and year ≥ 1990), set the system time to that value.
3. Other time sources (NTP, RTC, GPS, etc.) may update the system clock independently after mount. The filesystem does not manage those — it only provides this fallback to maintain chronological ordering.

### Timestamps During Writes

- **New file:** `ctime` is written to entry during allocation, set to current system time. `mtime` is left as 0xFFFFFFFF and written at `close()` time.
- **Update/append:** `ctime` is preserved from the old entry. `mtime` is left as 0xFFFFFFFF and written at `close()` time.
- **Rename:** `ctime` is preserved from the old entry. `mtime` is written before the ACTIVE → VALID transition within the `rename()` call.

---

## Allocation Strategy

### Entry Table Allocation

To allocate a new entry slot:

1. Start scanning from `next_free_entry` index.
2. For each slot:
   - If `state == FREE (0xFF)`: Read the full 32-byte slot.
     - If all 32 bytes are 0xFF: **use this slot**. Update `next_free_entry = slot_index + 1`.
     - If any non-0xFF byte exists (corrupted FREE slot): Write `state = ERASED (0x00)`. Skip this slot. Continue scanning.
   - If `state` is any other value: skip.
3. If the end of the entry table is reached without finding a free slot: perform **entry table defragmentation** (see below), then retry from step 1.
4. If still no free slot after defragmentation: return `ENOSPC`.

### Entry Table Defragmentation

When the entry table is exhausted (no FREE slots remain):

1. **Acquire the filesystem mutex.**
2. Identify all VALID and ACTIVE entries and their slot indices.
3. For each VALID/ACTIVE entry (in order), allocate a new sequential slot starting from slot 0:
   a. If the target slot is already this same VALID/ACTIVE entry, skip (no move needed).
   b. Write a new entry at the target slot with state = ALLOCATING.
   c. Copy all metadata fields from the old entry.
   d. Set `old_entry` field in the new entry to point to the old slot index.
   e. Transition new entry to PENDING_DATA.
   f. Transition new entry to TOMBSTONING_OLD.
   g. Write old entry state = TOMBSTONE.
   h. Write new entry state = **the source entry's original state**: VALID (0x07) if source was VALID, ACTIVE (0x0F) if source was ACTIVE. Do **not** promote ACTIVE to VALID — the entry's mtime/size may still be unfinalised.
   i. **Update open file descriptors:** Scan all open FDs. If any FD's `entry_index` matches the old slot index, update `fd.entry_index` to the new slot index. (The `flash_offset` and `data_size` are unchanged since the data region is not moved.)
4. After all VALID/ACTIVE entries are compacted to the front of the table, the remaining slots are a mix of TOMBSTONE, ERASED, and BADBLOCK entries.
5. For each 4 KB header page that contains **only** TOMBSTONE/ERASED/FREE entries (no VALID, ACTIVE, or BADBLOCK):
   a. **Erase that 4 KB page.** All slots in the page become FREE (0xFF).
   b. If erase verification fails (any byte not 0xFF after erase): mark the entire page's slots as BADBLOCK.
6. Update `next_free_entry` to the first FREE slot after the compacted VALID/ACTIVE entries.

**ACTIVE entries during defrag:** ACTIVE entries are moved as-is without running ACTIVE recovery (size scan-back, mtime repair). Their state is preserved. If the owning FD is still open, `close()` will finalize mtime/size and transition to VALID normally. If the entry is orphaned (FD was lost due to a prior crash), boot-time ACTIVE recovery will handle it.

### Data Region Allocation

#### Step 1: Find Contiguous Free Space

1. Scan the data region starting from the end of the last known allocation.
2. Look for a contiguous block of erased (0xFF) flash that is:
   - 4 KB-aligned at its start
   - Large enough for the file's total size (`entry.size`), rounded up to the next 4 KB boundary
3. If found → proceed to Step 2a.
4. If not found → proceed to Step 2b.

#### Step 2a: Allocate and Write to Free Space

1. **Verify** the candidate region is fully erased (read-back and confirm all 0xFF).
   - If any bytes are not 0xFF, erase the non-erased 4 KB blocks within the region.
   - Read-back again. If **any single block** still has non-0xFF bytes after erase: mark that block as BADBLOCK (allocate an entry with state = BADBLOCK pointing to that offset). Skip this region and retry from Step 1.
2. Proceed with the write flow (entry allocation, metadata, data).

#### Step 2b: Reclaim Tombstoned Regions

1. Build a free-space map: merge contiguous ranges of erased flash and TOMBSTONE entries' data regions.
2. Search for a contiguous range (erased + tombstoned) large enough for the file.
3. If found:
   - Erase the 4 KB blocks that belong to TOMBSTONE entries within this range.
   - Verify erase (single-failure → BADBLOCK, same as Step 2a).
   - Proceed to Step 2a.
4. If not found → proceed to Step 3.

#### Step 3: Data Region Defragmentation

Defragmentation reclaims fragmented free space by compacting VALID and ACTIVE files toward the beginning of the data region. It uses a single 4 KB RAM buffer — no file-sized allocations are required.

**Algorithm: Forward Compaction**

1. Collect all VALID and ACTIVE entries and sort by `offset` (ascending).
2. Set `compact_cursor = 0x10000` (start of data region).
3. For each file in offset order:
   a. Calculate `file_alloc = ceil(entry.size / 4096) * 4096`.
   b. If `file.offset == compact_cursor`: file is already in position. Advance `compact_cursor += file_alloc`. Continue.
   c. If `compact_cursor + file_alloc <= file.offset` (file fits entirely in the gap — no overlap with source):
      - Erase any non-erased 4 KB blocks in the destination range `[compact_cursor, compact_cursor + file_alloc)`.
      - Copy file data block-by-block (4 KB at a time) from `file.offset` to `compact_cursor`.
      - Allocate a new entry via TOMBSTONING_OLD flow with `offset = compact_cursor`.
      - Erase the old data blocks that are no longer covered by the new position.
      - Advance `compact_cursor += file_alloc`.
   d. If `compact_cursor + file_alloc > file.offset` (destination would overlap source):
      - Leave this file in place for this pass.
      - Advance `compact_cursor = file.offset + file_alloc`.
4. If any files were moved in this pass, repeat from step 1 (relocated files free up space that may resolve previously-skipped overlaps).
5. If no files were moved, compaction is complete.
6. After compaction, retry allocation from Step 1.

**Crash safety:** Each file move is atomic at the entry level. The source data is never modified until after the new entry is committed via TOMBSTONING_OLD. If power is lost:
- During block copy: old entry is still VALID/ACTIVE, pointing to original data. Partially-written destination blocks are orphaned and reclaimable.
- During entry update: TOMBSTONING_OLD recovery completes the transition.
- During old block erasure: old entry is already tombstoned. Blocks are safely reclaimable.

**RAM usage:** One 4 KB buffer for block-by-block copy. No file-sized allocations.

**Convergence note:** Each pass moves at least one file or terminates. In the pathological case where every gap is smaller than the file that follows it, no files move and the algorithm falls through to Step 4. In practice, this is rare on a filesystem with 4 KB alignment and typical file counts (< 50).

#### Step 4: No Space

- Return `ENOSPC`.
- The write operation fails; any existing file remains unchanged.

### 4 KB Alignment

All files start at 4 KB-aligned offsets within the data region:

```
┌─────────────────────────────────┐
│ File A                          │  Offset: 0x10000 (aligned)
│ [filename: L bytes]             │
│ [file data: N bytes]            │
│ [padding to 4 KB boundary]      │
├─────────────────────────────────┤  Offset: 0x10000 + ceil((L+N)/4096)*4096
│ File B                          │
│ ...                             │
├─────────────────────────────────┤
│ [Erased — free space]          │
└─────────────────────────────────┘
```

---

## File Operations (VFS Integration)

### Concurrency Model

MMROFS uses a single **recursive mutex** (`mmrofs_mutex`) to serialize all write operations:

| Operation | Locking |
|-----------|---------|
| `open()` (read) | Lock during entry lookup; release after FD allocated |
| `open()` (write/create) | Lock during entry lookup + allocation; release after FD allocated |
| `read()` | No lock required (mmap is read-only, data region is immutable once ACTIVE) |
| `write()` | Lock held for entire write (flash write must be atomic per-call) |
| `close()` (read) | No lock needed; just deallocate FD |
| `close()` (write) | Lock held for mtime write, size finalization, and ACTIVE → VALID transition |
| `unlink()` | Lock held for state transition |
| `rename()` | Lock held for entire operation |
| `stat()` / `fstat()` | Lock during entry lookup / data read |
| `opendir()` / `readdir()` | Lock during scan |

**Why a single mutex:** The filesystem targets embedded systems with < 50 files and low concurrency. A single mutex is simple, correct, and sufficient. The mutex is recursive to allow internal helper functions to re-acquire it.

### File Descriptor Structure

```c
struct mmrofs_fd_t {
    bool     in_use;         // FD slot is allocated
    uint16_t entry_index;    // Entry table slot index (0xFFFF if not yet allocated)
    uint32_t name_hash;      // FNV-1a hash of filename
    uint16_t name_len;       // Filename length
    char     filename[MAX_FILENAME_LEN]; // Filename copy (for deferred entry allocation)
    uint32_t data_offset;    // Current read position within file data
    uint32_t flash_offset;   // Partition-relative offset of file's data region
    uint32_t data_size;      // Read: entry.size - name_len. Write: bytes of data written so far.
    uint8_t  flags;          // O_RDONLY, O_WRONLY, O_RDWR, etc.
    uint8_t  fd_state;       // PENDING_NEW, PENDING_UPDATE, or COMMITTED
    uint16_t old_entry_index;// For updates: index of the old VALID/ACTIVE entry
    uint32_t old_data_size;  // For updates: exact data size of old entry (entry.size - name_len)
    uint32_t old_ctime;      // Preserved ctime for updates
};
```

**Max open FDs:** Configured at runtime via the `max_files` field of `mmrofs_mount_cfg_t` (passed to `mmrofs_register_vfs()`). The FD table is dynamically allocated on mount.

### Supported Operations

#### `open(path, flags)`

**Flags supported:** `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC` (implicit with `O_WRONLY | O_CREAT`)

1. Acquire mutex.
2. Parse filename from path (strip mount prefix).
3. Validate: `name_len >= 1`.
4. Compute `hash = fnv1a32(filename, name_len)`.
5. Scan **entire** entry table for a VALID or ACTIVE entry matching hash, name_len, and filename (memcmp via data region read).
6. Allocate an FD slot. If none available: release mutex, return `ENFILE`.

**If `O_RDONLY`:**
- File must exist → if not found, return `ENOENT`.
- Set `fd.data_offset = 0`, `fd.flash_offset = entry.offset`, `fd.data_size = entry.size - entry.name_len`.
- Release mutex.

**If `O_WRONLY | O_CREAT` or `O_RDWR | O_CREAT`:**
- If file exists:
  - `fd.fd_state = PENDING_UPDATE`, `fd.old_entry_index = existing_index`, `fd.old_ctime = existing_ctime`.
  - If old entry is VALID or ACTIVE with exact size (`(entry.size & 0xFFF) != 0xFFF`): `fd.old_data_size = entry.size - entry.name_len`.
  - If old entry is ACTIVE with capacity mask (`(entry.size & 0xFFF) == 0xFFF`): `fd.old_data_size` is unknown — set to `0xFFFFFFFF` (sentinel). Write operations will return `EIO` on append/rewrite attempts (see `write()`).
- If file does not exist: `fd.fd_state = PENDING_NEW`.
- Store filename, hash, name_len in FD.
- Release mutex.

**If `O_RDWR` (without O_CREAT):**
- File must exist → if not found, return `ENOENT`.
- `fd.fd_state = PENDING_UPDATE`, store old entry info.
- Set read position to 0.
- Release mutex.

#### `write(fd, data, len)`

1. Acquire mutex.
2. **First write to this FD:**
   a. **If `PENDING_UPDATE` and append is possible** (old data size is known AND space after existing data is free/tombstoned):
      - Determine `old_data_size`: if old entry is VALID, use `old_entry.size - old_entry.name_len`.
        If old entry is ACTIVE with unfinalised size and no FD is tracking the actual data size, append is not possible — skip to (b) or return `EIO`.
      - Allocate new entry slot: ALLOCATING → write metadata (size = name_len + old_data_size + len if known, or capacity mask) → PENDING_DATA.
      - Write new data at `old_entry.offset + name_len + old_data_size`.
      - PENDING_DATA → TOMBSTONING_OLD → tombstone old entry → ACTIVE.
      - `fd.flash_offset = old_entry.offset`, `fd.data_size = old_data_size + len`.
   b. **If `PENDING_UPDATE` and append not possible** (or old data size unknown):
      - If old entry is ACTIVE with unfinalised size and no known `old_data_size`: return `EIO`.
      - Find new data region space (Step 1 → 2a → 2b → 3 → 4).
      - Allocate new entry slot: ALLOCATING → write metadata (size = name_len + total_data if known, or capacity mask) → PENDING_DATA.
      - Write filename + copy old data + write new data.
      - PENDING_DATA → TOMBSTONING_OLD → tombstone old entry → ACTIVE.
      - `fd.flash_offset = new_offset`, `fd.data_size = old_data_size + len`.
   c. **If `PENDING_NEW`:**
      - Find data region space (Step 1 → 2a → 2b → 3 → 4).
      - Allocate entry slot: ALLOCATING → write metadata (size = name_len + data_size if known, or 0xFFF for streaming) → PENDING_DATA.
      - Write filename + data.
      - PENDING_DATA → ACTIVE.
      - `fd.flash_offset = offset`, `fd.data_size = len`.
   d. Set `fd.fd_state = COMMITTED`, update `fd.entry_index`.
3. **Subsequent writes (fd_state == COMMITTED):**
   a. Check capacity: `fd.name_len + fd.data_size + len > ceil(entry.size / 4096) * 4096`?
   b. **If within capacity:**
      - Write data to flash at `fd.flash_offset + fd.name_len + fd.data_size`.
      - `fd.data_size += len`.
   c. **If capacity exceeded (streaming expansion):**
      - Determine next capacity tier: `new_size = ((fd.name_len + fd.data_size + len) | 0xFFF)`.
        E.g., currently at 0xFFF (4 KB), expand to 0x1FFF (8 KB).
      - Check if the next block(s) at `fd.flash_offset + current_alloc` are free.
      - If yes: allocate new entry with same offset, `size = new_size`.
        ALLOCATING → metadata → PENDING_DATA → TOMBSTONING_OLD → tombstone old → ACTIVE.
      - If no: full rewrite to new location with sufficient capacity.
        Copy existing data (name + data) block-by-block, write new data.
        ALLOCATING → metadata → PENDING_DATA → TOMBSTONING_OLD → tombstone old → ACTIVE.
      - Update `fd.entry_index`, `fd.flash_offset` (if relocated).
      - Write data to flash at `fd.flash_offset + fd.name_len + fd.data_size`.
      - `fd.data_size += len`.
4. Release mutex.
5. Return bytes written or error.

**Known-size writes:** When the caller knows the total data size upfront, `entry.size` is written as the exact value (`name_len + data_size`) during ALLOCATING. No capacity mask is used and no expansion is needed. This is the common case in embedded systems.

**Streaming writes:** When the total size is unknown, the 12-bit capacity mask scheme is used (see Streaming Allocation). The entry is ACTIVE after the first write, and capacity is expanded block-by-block as needed via TOMBSTONING_OLD. The entry transitions to VALID only at `close()`.

#### `read(fd, buf, len)`

1. Compute flash read offset: `fd.flash_offset + fd.name_len + fd.data_offset`.
2. Read `min(len, fd.data_size - fd.data_offset)` bytes via mmap window.
3. Advance `fd.data_offset`.
4. Return bytes read.

No mutex required — the data region for VALID and ACTIVE entries is immutable once written, and reads go through the mmap window which handles its own mapping.

#### `lseek(fd, offset, whence)`

Supported for **read and read-write** FDs:

| Whence | Behavior |
|--------|----------|
| `SEEK_SET` | `fd.data_offset = offset` |
| `SEEK_CUR` | `fd.data_offset += offset` |
| `SEEK_END` | `fd.data_offset = fd.data_size + offset` |

Returns the new offset, or `EINVAL` if the resulting position is negative or beyond `fd.data_size`.

**Not supported** for write-only FDs (writes are always sequential/appending).

#### `close(fd)`

1. **If read-only FD:** Deallocate FD slot. Return 0.
2. **If write FD:**
   a. Acquire mutex.
   b. If `fd.fd_state == COMMITTED` (entry exists on flash):
      - Write `mtime` to the entry: `entry.mtime = current system time`.
        Since mtime was left as 0xFFFFFFFF during allocation, this clears bits — flash-safe.
        **Torn write risk:** If power dies mid-write, the 4-byte mtime may be partially programmed (neither 0xFFFFFFFF nor valid). ACTIVE recovery handles this by detecting `mtime != 0xFFFFFFFF` and allocating a new entry.
      - Write the exact `size` to the entry's size field: `size = fd.name_len + fd.data_size`.
        This clears the bottom 12 bits from the capacity mask to the actual value (flash-safe: only clears bits).
        If the size was already written exactly (known-size write), this is a no-op.
        **Torn write risk:** If power dies mid-write, the 4-byte size may be partially programmed. ACTIVE recovery handles this by scanning backwards for trailing 0xFF to validate or correct the size.
      - Write `state = VALID (0x07)`. Entry is now fully committed.
   c. If `fd.fd_state == PENDING_NEW` or `PENDING_UPDATE` (no write() was ever called):
      - No entry was allocated. Nothing to finalize.
   d. Deallocate FD slot.
   e. Release mutex.
   f. Return 0.

#### `unlink(path)`

1. Acquire mutex.
2. Lookup file (full entry table scan).
3. If not found: release mutex, return `ENOENT`.
4. Write entry state = TOMBSTONE (0x03).
5. Release mutex.
6. Return 0.

#### `rename(src, dst)`

1. Acquire mutex.
2. Lookup `src` entry. If not found: return `ENOENT`.
3. Lookup `dst` entry. Record `dst_index` if found; otherwise `dst_index = 0xFFFFFFFF`.
4. Allocate new data region space for `dst` (total = new_name_len + data_size from src).
5. Allocate new entry slot:
   a. Write state = ALLOCATING.
   b. Write metadata: new name_hash, new name_len, new offset, size = new_name_len + (src.size - src.name_len), preserve ctime from src entry, old_entry = src entry index, dst_entry = dst_index. Leave mtime as 0xFFFFFFFF.
   c. Write state = PENDING_DATA.
6. Write `dst` filename to new data region.
7. Copy file data from `src` data region to new data region.
8. Write new entry state = TOMBSTONING_OLD (0x1F).
9. Write `src` entry state = TOMBSTONE (0x03).
10. If `dst_index != 0xFFFFFFFF`: write `dst` entry state = TOMBSTONE (0x03).
11. Write `mtime` to new entry (current system time).
12. Write new entry state = ACTIVE (0x0F).
13. Write new entry state = VALID (0x07).
14. Release mutex.
15. Return 0.

**Rename atomicity:** The destination is never tombstoned before the new entry reaches TOMBSTONING_OLD. If power is lost before step 8, the new entry is in ALLOCATING or PENDING_DATA — recovery tombstones it and both `src` and `dst` are untouched. If power is lost at or after step 8, TOMBSTONING_OLD recovery completes both tombstones and promotes the new entry.

#### `stat(path, st)` / `fstat(fd, st)`

1. Acquire mutex.
2. Lookup entry (by path or fd).
3. Populate `struct stat`:
   - `st_size = entry.size - entry.name_len` (data only, excludes filename)
   - `st_mode = S_IFREG | 0444` (read-only regular file)
   - `st_mtime = entry.mtime`
   - `st_ctime = entry.ctime`
4. Release mutex.

#### `opendir(path)` / `readdir(dir)` / `closedir(dir)`

- `opendir`: Allocate directory handle, set scan index to 0.
- `readdir`: Scan entry table from current index. For each VALID or ACTIVE entry, read filename from data region, populate `dirent`, advance index. Return NULL at end.
- `closedir`: Deallocate handle.

### Unsupported Operations

- Directories (flat namespace only)
- Sparse files
- Symlinks / hard links
- Hierarchical paths (no subdirectories)
- Extended attributes (xattr)
- Access control lists (ACL)
- `lseek` on write-only FDs

---

## Hash Computation

**Algorithm:** FNV-1a 32-bit

```c
uint32_t fnv1a32(const char *data, size_t len) {
    uint32_t hash = 2166136261u;  // FNV offset basis
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;        // FNV prime
    }
    return hash;
}
```

**Lookup guarantees:**
- Hash mismatch → skip entry immediately (fast reject)
- Hash match → verify with `memcmp(stored_name, search_name, name_len)` for exact match
- Hash collisions are handled by the memcmp fallback

---

## File Lookup

```
1. Compute hash: h = fnv1a32(filename, len)
2. Scan ENTIRE entry table (slot 0 to max_entries - 1):
   - Skip if state != VALID (0x07) && state != ACTIVE (0x0F)
   - Skip if entry.name_hash != h
   - Skip if entry.name_len != len
   - **Validate entry** (offset, size, alignment, bounds — see Entry Validation).
     If invalid: tombstone entry in-place, skip.
   - Read filename from data region (at entry.offset, length entry.name_len)
   - Skip if memcmp(stored_name, filename, len) != 0
   - Return entry index
3. If no match found: return -ENOENT
```

The scan does **not** stop at FREE entries. This is necessary because header page erasure can create FREE gaps in the middle of the table.

**Complexity:** O(N) where N = total entry slots. With hash pre-filtering, actual filename reads are rare.

---

## Entry Validation

Before trusting any ACTIVE, VALID, or TOMBSTONING_OLD entry, the following checks **must** pass. If any check fails, the entry is corrupt and must be tombstoned (write `state = TOMBSTONE (0x03)`).

### Validation Checks

| Check | Rule | Rationale |
|-------|------|-----------|
| `name_len` range | `1 ≤ name_len ≤ MAX_FILENAME_LEN` | Zero is invalid; protects against underflow in `size - name_len` |
| `size ≥ name_len` | `size >= name_len` (or if capacity mask: `allocated_bytes >= name_len`) | Otherwise `data_size = size - name_len` underflows |
| `offset` minimum | `offset >= 0x10000` | Data must be in data region, not header |
| `offset` alignment | `offset % 4096 == 0` | All files are 4 KB-aligned |
| `offset + size` bounds | `offset + allocated_bytes <= partition_size` | Prevents reads/writes past partition end. For capacity-mask entries, use `allocated_bytes = ceil(size / 4096) * 4096` |
| `old_entry` range | `old_entry == 0xFFFFFFFF` or `old_entry < max_entries` | Prevents out-of-bounds slot access |
| `dst_entry` range | `dst_entry == 0xFFFFFFFF` or `dst_entry < max_entries` | Prevents out-of-bounds slot access |

### When Validation Is Applied

1. **Recovery (boot-time):** Before processing any TOMBSTONING_OLD, ACTIVE, or VALID entry, run all checks. If validation fails, tombstone the entry immediately — do not attempt recovery logic.
2. **File lookup:** After hash/name_len match but **before** reading the filename from flash, validate `offset` and `size`. This prevents a single-bitflip in `offset` from causing an out-of-bounds read.
3. **open() for read:** Validate the matched entry before populating the FD. Return `EIO` if validation fails.

A single-bitflip in `offset` or `size` can send reads outside the partition boundary. Validating before every data-region access is the only defence.

---

## Garbage Collection

### Header Page Erasure

A 4 KB header page can be erased when **all** entries in that page are TOMBSTONE, ERASED, or FREE (no VALID, ACTIVE, ALLOCATING, PENDING_DATA, TOMBSTONING_OLD, or BADBLOCK entries):

```
for each 4 KB page in header region:
    if all entries in page are {TOMBSTONE, ERASED, FREE}:
        erase_4KB_page()
        // All slots become FREE (0xFF)
        // Update next_free_entry if this page is earlier
```

### Data Region Erasure

Data blocks belonging to TOMBSTONE entries can be erased opportunistically (e.g., during allocation when scanning for free space) or during defragmentation. No separate GC pass is required.

---

## BADBLOCK Handling

### Detection

A block is marked BADBLOCK after a **single** erase-verify failure:
1. Issue 4 KB erase command.
2. Read back the entire 4 KB block.
3. If any byte is not 0xFF → mark as BADBLOCK.

### Marking

- For **data region** blocks: Allocate an entry with `state = BADBLOCK`, `offset` = the bad block's offset, `size = 4096`.
- For **header region** blocks: Write `state = BADBLOCK` to all entry slots within that 4 KB page.

BADBLOCK entries are permanent and survive defragmentation. They are never erased or reclaimed.

---

## Constraints & Limits

| Parameter | Value | Notes |
|-----------|-------|-------|
| Byte order | Little-endian | All multi-byte fields |
| Max filename length | 65535 bytes | 2-byte `name_len` field |
| Min filename length | 1 byte | `name_len = 0` is invalid |
| Max entry slots | 1875 | 60 KB usable / 32 bytes per entry |
| Max partition size | 4 GB | 32-bit offset field |
| Header size | 64 KB | Fixed |
| File alignment | 4 KB | All files start at 4 KB boundary |
| Entry struct size | 32 bytes | Fixed; includes mtime, ctime, old_entry, dst_entry |
| Size field | name_len + data | Includes filename; bottom 12 bits = capacity mask for streaming |
| Erase block | 4 KB | Hardware minimum erase granularity |
| Mmap window | 64 KB | Sliding read window |
| Max open FDs | Configurable | `mmrofs_mount_cfg_t.max_files`, set at runtime |
| BADBLOCK threshold | 1 failure | Single erase-verify failure → permanent BADBLOCK |

---

## Example: Writing a New File

```
Scenario: Write "Hello World" to /mm/demo.txt (known size)

1. open("/mm/demo.txt", O_WRONLY | O_CREAT, 0)
   - filename = "demo.txt" (8 bytes)
   - hash = fnv1a32("demo.txt") = 0xDEADBEEF (example)
   - Scan entry table: no VALID or ACTIVE entry with this hash → PENDING_NEW
   - Allocate FD

2. write(fd, "Hello World", 11)
   First write (known size = 8 + 11 = 19):
   - Find free data region, e.g., offset 0x20000
   - Allocate entry slot (e.g., slot 5):
     - Write state = ALLOCATING (0x7F)
     - Write name_len=8, name_hash=0xDEADBEEF, offset=0x20000, size=19,
       mtime=0xFFFFFFFF, ctime=1740441600, old_entry=0xFFFFFFFF
     - Write state = PENDING_DATA (0x3F)
   - Write filename at 0x20000:
     - "demo.txt" (8 bytes)
   - Write file data at 0x20000 + 8:
     - "Hello World" (11 bytes)
   - Write state = ACTIVE (0x0F) → file now visible

3. close(fd)
   - Write mtime = 1740441600 to entry
   - size already exact (19) → no-op for size finalization
   - Write state = VALID (0x07) → file fully committed

Entry slot 5: state=0x07, name_len=8, hash=0xDEADBEEF, offset=0x20000, size=19,
  mtime=1740441600, ctime=1740441600, old_entry=0xFFFFFFFF
Flash at 0x20000: [demo.txt][Hello World]
```

---

## Example: Append to Existing File

```
Scenario: Append " - Goodbye" (10 bytes) to /mm/demo.txt

1. open("/mm/demo.txt", O_WRONLY | O_CREAT)
   - Scan: find VALID entry at slot 5, size=19 (8 name + 11 data)
   - fd_state = PENDING_UPDATE, old_entry_index = 5

2. write(fd, " - Goodbye", 10)
   First write (known total = 8 + 11 + 10 = 29):
   - Check space after existing file:
     - File occupies 0x20000 to 0x20000+19-1 = 0x20012
     - Allocated block: 0x20000-0x20FFF (4 KB)
     - Remaining in block: 4096 - 19 = 4077 bytes → plenty for 10 bytes
   - Append flow:
     - Allocate new entry slot (e.g., slot 6):
       - Write state = ALLOCATING (0x7F)
       - Write name_len=8, hash=0xDEADBEEF, offset=0x20000, size=29 (8+11+10),
         mtime=0xFFFFFFFF, ctime=1740441600 (preserved from slot 5), old_entry=5
       - Write state = PENDING_DATA (0x3F)
     - Write " - Goodbye" at 0x20000 + 19 = 0x20013
     - Write entry 6 state = TOMBSTONING_OLD (0x1F)
     - Write entry 5 state = TOMBSTONE (0x03)
     - Write entry 6 state = ACTIVE (0x0F)
     - fd_state = COMMITTED

3. close(fd)
   - Write mtime = 1740528000 to entry 6
   - size already exact (29) → no-op for size finalization
   - Write entry 6 state = VALID (0x07) → file fully committed

Entry slot 5: state=0x03 (TOMBSTONE)
Entry slot 6: state=0x07, name_len=8, hash=0xDEADBEEF, offset=0x20000, size=29,
  mtime=1740528000, ctime=1740441600, old_entry=5
```

---

## Example: Streaming Write (Unknown Size)

```
Scenario: Stream data to /mm/log.bin in chunks of unknown total size

1. open("/mm/log.bin", O_WRONLY | O_CREAT, 0)
   - filename = "log.bin" (7 bytes)
   - hash = fnv1a32("log.bin") = 0xCAFEBABE (example)
   - No existing entry → PENDING_NEW

2. write(fd, data_chunk_1, 2000)  — first write, 2000 bytes
   First write (streaming, unknown size):
   - Find free data region, e.g., offset 0x30000
   - Allocate entry slot (e.g., slot 10):
     - Write state = ALLOCATING (0x7F)
     - Write name_len=7, hash=0xCAFEBABE, offset=0x30000, size=0x00000FFF,
       mtime=0xFFFFFFFF, ctime=1740441600, old_entry=0xFFFFFFFF
     - Write state = PENDING_DATA (0x3F)
   - Write filename at 0x30000: "log.bin" (7 bytes)
   - Write data at 0x30000 + 7: 2000 bytes
   - Write state = ACTIVE (0x0F) → file visible
   - fd_state = COMMITTED, data_size = 2000
   - Capacity: (0xFFF | 0xFFF) + 1 = 4096, used: 7 + 2000 = 2007, remaining: 2089

3. write(fd, data_chunk_2, 2000)  — second write, 2000 bytes
   - Check capacity: 7 + 2000 + 2000 = 4007 ≤ 4096 → fits
   - Write 2000 bytes at 0x30000 + 7 + 2000 = 0x307D7
   - data_size = 4000

4. write(fd, data_chunk_3, 500)  — third write, 500 bytes
   - Check capacity: 7 + 4000 + 500 = 4507 > 4096 → expansion needed!
   - Next tier: new_size = (4507 | 0xFFF) = 0x1FFF (8192 capacity)
   - Check block at 0x31000: free ✓
   - Allocate new entry slot (e.g., slot 11):
     - Write state = ALLOCATING (0x7F)
     - Write name_len=7, hash=0xCAFEBABE, offset=0x30000, size=0x00001FFF,
       mtime=0xFFFFFFFF, ctime=1740441600, old_entry=10
     - Write state = PENDING_DATA (0x3F)
   - PENDING_DATA → TOMBSTONING_OLD → tombstone slot 10 → ACTIVE
   - Write 500 bytes at 0x30000 + 7 + 4000 = 0x30FA7
   - data_size = 4500

5. close(fd)
   - Write mtime = 1740441600 to entry 11
   - Finalize size: write size = 7 + 4500 = 4507 = 0x0000119B
   - 0x00001FFF → 0x0000119B: only clears bits ✓
   - Write state = VALID (0x07) → file fully committed

Entry slot 10: state=0x03 (TOMBSTONE)
Entry slot 11: state=0x07, name_len=7, hash=0xCAFEBABE, offset=0x30000, size=4507,
  mtime=1740441600, ctime=1740441600, old_entry=10
```

---

## Example: Recovery After Power Loss

```
Before power loss:
  Entry slot 7: state=PENDING_DATA (0x3F), name="config.bin", offset=0x30000, size=0x00000FFF

Power loss occurs before state transition to ACTIVE.

On next boot, mmrofs_recover():
  - Scan slot 7: state = PENDING_DATA
  - Recovery rule: write state = TOMBSTONE (0x03)
  - File is NOT visible to the user
  - Data at 0x30000 is orphaned (reclaimable via GC)

Result: Safe. Partial write is rolled back. No corruption.
```

---

## Example: Recovery of TOMBSTONING_OLD (Rename Over Existing)

```
Before power loss:
  Entry slot 2: state=VALID, name="new.txt", offset=0x30000, size=57 (7+50) ← existing dst
  Entry slot 3: state=VALID, name="old.txt", offset=0x20000, size=107 (7+100) ← src
  Entry slot 8: state=TOMBSTONING_OLD (0x1F), name="new.txt", offset=0x40000, size=107 (7+100),
    old_entry=3, dst_entry=2

Power loss occurs after TOMBSTONING_OLD but before old→TOMBSTONE, dst→TOMBSTONE, and new→ACTIVE.

On next boot, mmrofs_recover():
  - Scan slot 8: state = TOMBSTONING_OLD
  - Validate entry (offset, size, alignment, bounds): OK
  - Read old_entry from entry struct at offset 24: value = 3
  - Entry 3 state = VALID → write entry 3 state = TOMBSTONE (0x03)
  - Read dst_entry from entry struct at offset 28: value = 2
  - Entry 2 state = VALID → write entry 2 state = TOMBSTONE (0x03)
  - Write entry 8 state = ACTIVE (0x0F)
  - ACTIVE recovery for slot 8:
    - Size recovery: scan backwards from offset 0x40000 + 4096 (allocated_bytes).
      Last non-0xFF byte at 0x4006A → inferred_size = 107.
      Current size = 107, inferred = 107, ≤2 trailing 0xFF → size is valid, leave as-is.
    - Mtime recovery: mtime = 0xFFFFFFFF → write current system time as mtime.
    - Write entry 8 state = VALID (0x07)
  - Rename completed successfully despite power loss. Both src and dst are tombstoned atomically.
```

---

## Performance Notes

- **Write latency:** ~100 ms per 4 KB block (flash-dependent)
- **Read latency:** < 1 ms for mmap-window hits
- **Lookup latency:** O(N) scan, < 1 ms for < 50 files (hash pre-filter minimizes data reads)
- **Recovery time:** < 500 ms (single pass over entry table)
- **Defragmentation:** Proportional to total data size; rare in typical use

---
