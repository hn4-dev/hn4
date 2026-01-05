# HN4 LIFECYCLE ARCHITECTURE
### *Format, Mount, Unmount & The Auto-Medic Boot*

**Specification:** v5.5 | **Type:** Architectural Guide
**Components:** `hn4_format.c`, `hn4_mount.c`, `hn4_unmount.c`

---

## 1. Executive Summary

In traditional file systems, the lifecycle phases are distinct and rigid: **Format** writes structures, **Mount** reads them, and **Fsck** fixes them.

In HN4, these boundaries are fluid.
*   **Format** does not just clear data; it establishes a **Cardinal Geometry** and pre-calculates ECC for empty space.
*   **Mount** is not a passive loader; it is an active **Repair Engine**. It performs "Transfusion Healing" on the allocation map as it loads it into RAM.
*   **Unmount** is a cryptographic seal, performing ALU integrity checks on the CPU itself before committing the final state to ensure data safety.

---

## 2. FORMAT: THE BIG BANG (`hn4_format.c`)

Formatting is the creation of the universe. HN4 uses a **Calculated Layout** rather than a static partition table.

### 2.1 The Cardinal Points (Quad-Redundancy)
To survive catastrophic head crashes or partition table overwrites, HN4 writes the **Superblock** (The DNA of the volume) to four mathematically distinct locations known as the Cardinal Points.

| Location | Name | Role | Offset Calculation |
| :--- | :--- | :--- | :--- |
| `0` | **North** | Primary | `LBA 0` |
| `33%` | **East** | Replica 1 | `Floor(Capacity * 0.33)` (Aligned) |
| `66%` | **West** | Replica 2 | `Floor(Capacity * 0.66)` (Aligned) |
| `100%` | **South** | Sentinel | `Capacity - 8KB` |

**The Logic:** Even if the beginning and end of the drive are wiped by a careless `dd` command, the East and West poles survive deep in the address space.

### 2.2 The Pre-Calc ECC Bitmap
Standard formatters write `0x00` to the allocation bitmap.
HN4 writes **Armored Words**.

*   **Structure:** `[ 64-bit Data | 8-bit ECC | Padding ]`
*   **Calculation:** The formatter pre-calculates `ECC(0)`.
*   **The Write:** It streams this pattern to the Bitmap Region (`LBA_BITMAP`).
*   **Benefit:** The drive is born "Armored." If a cosmic ray flips a bit in the free space map 1 second after formatting, the Allocator will detect it immediately because the ECC will mismatch.

### 2.3 The Wormhole Injection
(As defined in the Wormhole Protocol). During Format, the engineer can inject a specific `UUID` and `Mount_Intent`. This allows the creation of **Physical Clones** and **Ram-Drive Overlays** that share the same mathematical trajectory as an existing drive.

---

## 3. MOUNT: THE AWAKENING (`hn4_mount.c`)

The Mount process is designed to be **Zero-Allocation** (using a static 64KB Work Buffer) and **Self-Healing**.

### 3.1 The Quorum Vote
The driver reads all 4 Cardinal Superblocks into the Work Buffer.
It performs a **Leader Election**:
1.  **Validity Check:** Verify `CRC32` and `Magic_Number`.
2.  **Generation Check:** Highest `copy_generation` wins.
3.  **Tie-Breaker:** If generations are equal, prefer the one marked `VOL_CLEAN`.
4.  **Sync:** If the South pole is outdated (older generation), the driver queues a background job to overwrite it with the North pole's data.

### 3.2 The Epoch Rollback (Time Travel)
HN4 does not use a Journal. It uses an **Epoch Ring** (1MB Circular Log).
1.  **Scan:** The driver scans the 128 slots of the Epoch Ring.
2.  **Verify:** It checksums every 128-byte header.
3.  **Locate:** It finds the latest *valid* timestamp.
4.  **Rollback:** It sets the filesystem "Present" to that timestamp.
    *   *Effect:* Any "Torn Writes" (incomplete data from a power loss) that occurred *after* the last valid Epoch are mathematically discarded because they exist in the "Future" relative to the restored state.

### 3.3 Transfusion Healing (The Bitmap Loader)
**This is the core of Mount-Time Repair.**

Instead of blindly copying the Allocation Bitmap from Disk to RAM, the loader acts as a filter.

*   **Step 1:** Read 64KB Chunk from Disk.
*   **Step 2 (The Filter):** Iterate through every 128-bit Armored Word.
*   **Step 3 (ECC Check):**
    *   Calculate `ECC(Data)`. Compare with `Stored_ECC`.
    *   **Match:** Copy to RAM.
    *   **Mismatch:**
        *   **Action:** Perform Hamming Correction (Bit Flip).
        *   **Heal:** Write the *Corrected* value to RAM.
        *   **Flag:** Set `VOL_DIRTY`.
*   **Step 4:** Continue.

**The Result:** By the time `hn4_mount` returns `OK`, the **RAM Image** of the allocation table is 100% mathematically perfect, even if the **Disk Image** was rotting. The corruption has been "filtered out" during the load.

---

## 4. UNMOUNT: THE FREEZE (`hn4_unmount.c`)

Unmount is the critical phase where volatile state (RAM) is serialized to non-volatile storage (NAND).

### 4.1 The CPU Sanity Check
Before writing *anything* to disk, the driver tests the silicon it is running on.
*   **Logic:** `Assert( (A & B) | C == Expected )` and `Assert( CRC32("123") == Constant )`.
*   **Why:** If the CPU's ALU (Arithmetic Logic Unit) or Registers are corrupted (due to undervoltage or overheating), writing metadata would shred the filesystem.
*   **Failure:** If the check fails, the driver **PANICS** and refuses to unmount (halts), preserving the previous valid state on disk rather than overwriting it with garbage.

### 4.2 The Flush (Persistence)
1.  **Bitmap Flush:**
    *   The driver writes the `Void_Bitmap` from RAM to Disk.
    *   **Crucially:** Since the RAM image was "Healed" during Mount (see 3.3), this overwrite **permanently fixes** any bit rot on the physical media.
2.  **Epoch Advance:**
    *   The driver writes a final "Clean Epoch" to the ring.
3.  **Superblock Seal:**
    *   It updates `state_flags` to `VOL_CLEAN`.
    *   It increments `copy_generation`.
    *   It writes to all 4 Cardinal Points.

---

## 5. DEEP DIVE: MOUNT SELF-REPAIR MECHANICS

The most advanced feature of HN4 is that it does not require an offline `fsck` (File System Check) to fix allocation errors. It uses **Lazy Repair**.

### The Scenario
A cosmic ray strikes the SSD while it is powered off. Bit 4096 in the Allocation Bitmap (representing 2MB of data) flips from `0` (Free) to `1` (Used).

### The Traditional Failure (ext4/NTFS)
1.  Mounts successfully (checksums might be ignored or cover large groups).
2.  OS tries to write to that area.
3.  Filesystem thinks it's used. Space is leaked.
4.  User must run `chkdsk /f` (hours of downtime) to find the leak.

### The HN4 Auto-Medic Sequence
1.  **Power On.**
2.  **Mount begins.** Driver streams the Bitmap into the Work Buffer.
3.  **Detection:**
    *   The Loader processes the word containing Bit 4096.
    *   `ECC_Calc` differs from `ECC_Stored`.
    *   Syndrome analysis identifies Bit 4096 as the error.
4.  **Correction (RAM):**
    *   The Loader flips Bit 4096 back to `0` in the **RAM Copy**.
    *   The `repair_needed` flag is raised.
5.  **Mount Completes.** The Volume is online. The OS sees the space as Free (Correct).
6.  **Persistence (The Cure):**
    *   **Option A (Clean Shutdown):** When the user unmounts, the Corrected RAM Bitmap overwrites the Corrupt Disk Bitmap.
    *   **Option B (Runtime Flush):** If `repair_needed` was set, the driver triggers a background thread to flush that specific bitmap page to disk immediately.

**Engineering Verdict:** The filesystem repaired physical corruption on the hard drive simply by reading it into memory and writing it back. **Zero Downtime.**