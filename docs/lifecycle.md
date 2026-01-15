# HN4 LIFECYCLE: MOUNT & RECOVERY ARCHITECTURE
**Status:** Implementation Standard v7.9
**Components:** `hn4_mount.c`, `hn4_unmount.c`, `hn4_format.c`
**Scope:** Volume Initialization, State Transition, and Error Recovery

---

## 1. Executive Summary

In HN4, the lifecycle phases (Format, Mount, Unmount) are tightly integrated with the self-healing and integrity mechanisms.

*   **Format:** Establishes the geometric layout and writes redundant Superblocks to calculated offsets (Cardinal Points).
*   **Mount:** Acts as a recovery engine. It validates the Superblock Quorum, verifies the Epoch Ring, and repairs in-memory structures (Bitmap ECC) during loading.
*   **Unmount:** Ensures atomic persistence. It flushes dirty metadata, advances the Epoch generation, and updates all Superblock replicas to a consistent `CLEAN` state.

---

## 2. FORMAT: Volume Initialization (`hn4_format.c`)

Formatting defines the physical geometry and initializes the integrity structures.

### 2.1 Redundant Superblocks (Cardinal Layout)
To ensure survivability against partial media failure, the Superblock is replicated at four deterministic locations based on the volume capacity.

| Location | Name | Logical Calculation |
| :--- | :--- | :--- |
| **0** | Primary (North) | `LBA 0` |
| **1** | Mirror 1 (East) | `Align_Up(Capacity * 0.33, BlockSize)` |
| **2** | Mirror 2 (West) | `Align_Up(Capacity * 0.66, BlockSize)` |
| **3** | Mirror 3 (South) | `Align_Down(Capacity - SB_Size, BlockSize)` |

*   **Logic:** `_calc_south_offset` handles alignment and small-volume edge cases.
*   **Constraint:** If `Capacity < 16 * SB_Size`, the South mirror is disabled (`HN4_COMPAT_SOUTH_SB` flag cleared).

### 2.2 Metadata Initialization
*   **Zeroing:** Critical regions (Epoch Ring, Cortex, Bitmaps) are explicitly zeroed to prevent "Ghost" reads of stale data.
*   **Genesis Anchors:** The Root Anchor (ID `0xFF...FF`) and the initial Epoch Header (ID `1`) are written to their respective locations.
*   **Poisoning:** If the format operation fails mid-way, the Superblock locations are overwritten with `0xDEADBEEF` to prevent the volume from being mounted in an inconsistent state.

---

## 3. MOUNT: State Restoration (`hn4_mount.c`)

The mount process prioritizes data integrity over availability. It performs a "Cardinal Vote" to determine the authoritative state of the volume.

### 3.1 The Cardinal Vote (Quorum)
The driver reads Superblocks from all 4 locations into a work buffer.
1.  **Probe:** Reads Primary, East, West, and South.
2.  **Validation:** Checks `Magic`, `CRC32`, and `UUID` for each candidate.
3.  **Election:** Selects the Superblock with the highest `copy_generation`.
    *   **Tie-Breaker:** If generations match, prefer the one with the latest `last_mount_time`.
4.  **Split-Brain Detection:** If a candidate has the same Generation but a different UUID or Block Size, it flags `HN4_ERR_TAMPERED`.
5.  **Healing:** If `allow_repair` is true (RW mount), the driver overwrites any outdated mirrors with the elected Leader's data.

### 3.2 Epoch & Journal Verification
HN4 validates the temporal consistency of the volume using the **Epoch Ring**.
*   **Check:** `hn4_epoch_check_ring` reads the epoch entry pointed to by the Superblock.
*   **Skew Detection:** Calculates `delta = Disk_Epoch_ID - Memory_Epoch_ID`.
    *   If `delta < 0` (Past): **Generation Skew** (Potential lost write).
    *   If `delta > 0` (Future): **Time Dilation** (Superblock is stale relative to Ring).
*   **Policy:** Significant skew forces a Read-Only mount to prevent logical corruption.

### 3.3 Load-Time ECC Repair (The Bitmap Loader)
When loading the Allocation Bitmap from disk to RAM (`_load_bitmap_resources`), the driver performs active error correction.

1.  **Read:** Fetches raw bitmap data from disk.
2.  **Verify:** For each `hn4_armored_word_t`:
    *   Calculates SECDED syndrome for the 64-bit data.
    *   Compares against the stored 8-bit ECC.
3.  **Correct:** If a Single Bit Error (SEC) is detected:
    *   The bit is flipped in the **RAM copy**.
    *   The `out_was_corrected` flag is raised.
4.  **Result:** The in-memory bitmap is clean. The on-disk corruption is healed when the dirty bitmap is flushed at unmount/sync.

---

## 4. UNMOUNT: Atomic Persistence (`hn4_unmount.c`)

Unmount transitions the volume from a volatile (RAM) state to a consistent non-volatile (Disk) state.

### 4.1 Persistence Sequence
The unmount sequence (`hn4_unmount`) enforces a strict ordering to guarantee consistency:

1.  **Data Flush:** `HN4_IO_FLUSH` ensures all user data is stable on media.
2.  **Metadata Flush:** Writes the (potentially healed) Allocation Bitmap and Quality Mask to disk.
3.  **Epoch Advance:**
    *   Writes a new Epoch Header with `HN4_VOL_UNMOUNTING` flag.
    *   This acts as a "Write Barrier" for the journal.
4.  **Superblock Broadcast:**
    *   Updates `state_flags` to `HN4_VOL_CLEAN`.
    *   Increments `copy_generation`.
    *   Writes the updated Superblock to all 4 Cardinal Points.
5.  **Final Barrier:** Issues a hardware flush to commit the Superblocks.

### 4.2 Failure Handling
If any step fails (e.g., IO Error during SB write):
*   The driver reverts the in-memory state to `HN4_VOL_DIRTY` or `HN4_VOL_DEGRADED`.
*   It attempts to write the "Dirty" state to disk to reflect the failure.
*   This ensures the next Mount will trigger a full recovery/check.

---

## 5. Self-Healing Mechanics

HN4 leverages the Mount/Unmount cycle to repair latent corruption without offline tools.

### Scenario: Bit Rot in Allocation Map
1.  **Corruption:** A bit in the bitmap on disk flips (0 -> 1).
2.  **Mount:** `_load_bitmap_resources` detects the ECC mismatch.
3.  **Repair:** The driver corrects the bit in RAM (1 -> 0) using the Hamming Code.
4.  **Operation:** The filesystem operates with the correct (clean) bitmap.
5.  **Unmount:** The clean bitmap from RAM overwrites the corrupt bitmap on disk.
    *   **Result:** The corruption is permanently fixed.

### Scenario: Stale Mirror
1.  **Corruption:** The "East" Superblock was not updated during the last shutdown (power loss).
2.  **Mount:** The Quorum logic detects "North" has Generation 100, but "East" has Generation 99.
3.  **Election:** "North" wins (Gen 100).
4.  **Repair:** The driver enters the healing phase (`_execute_cardinal_vote`) and overwrites "East" with the data from "North".
    *   **Result:** Redundancy is restored transparently.
