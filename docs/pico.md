# HN4 PICO PROFILE
### *The Minimalist Manifold: From Microcontrollers to Floppy Disks*

**Specification:** v9.1 (Embedded) | **Target:** MCU, RTOS, Legacy Media | **Memory Footprint:** < 4KB RAM

---

> **🎯 Design Goals**
> *   **Constant-Time Addressing:** Zero dependency on FAT table traversal or B-Tree depth.
> *   **Crash-Safe Writes:** Atomic "Shadow Hop" updates without the overhead of journaling.
> *   **Minimal Footprint:** Total RAM requirement strictly < 4KB.
> *   **Deterministic Behavior:** Bounded CPU cycles and predictable timing for RTOS integration.
> *   **Universal Media:** Functions correctly on slow, high-latency media (Floppy, EEPROM) and modern Flash.

---

## 1. Executive Summary

While the standard HN4 architecture scales to Exabytes, the **Pico Profile** (`HN4_PROFILE_PICO`) is a specialized subset designed for extreme resource constraints. It retains the core mathematical properties of the Void Engine (Ballistic Addressing, Atomic Relocation) but strips away scalability features (Horizon Log, Extension Chains, Redundant Superblocks) to fit within the SRAM of a Microcontroller Unit (MCU).

This profile enables **high-integrity, power-fail-safe file storage** on devices ranging from 32-bit Cortex-M0 sensors to legacy 1.44MB Floppy Disks, eliminating the corruption risks inherent in FAT12/FAT16.

**Key Constraints:**
*   **Minimum RAM:** 4KB (Static Allocation)
*   **Code Size:** < 16KB (Thumb-2)
*   **Media Support:** NOR Flash, NAND, EEPROM, Floppy, SD Card (SPI Mode)

---

## 2. Profile Comparison: Standard vs. Pico

| Feature | Standard HN4 | Pico Profile | User Impact |
| :--- | :--- | :--- | :--- |
| **Addressing Math** | 128-bit Ballistic | 32-bit Ballistic | Limited to 2TB Volumes. |
| **Collision Fallback** | Horizon Log (D2) | **Failure (ENOSPC)** | Deterministic failure instead of unpredictable latency spikes. |
| **Metadata Resilience** | 4-Way Cardinal Replicas | **Single Superblock** | Volume unreadable if Sector 0 dies. |
| **Integrity Check** | CRC32C + Hamming ECC | **CRC16** | Detects random errors; vulnerable to multi-bit bursts. |
| **RAM Footprint** | Dynamic (MBs to GBs) | **Static (< 4KB)** | Fits on smallest Cortex-M0/AVR parts. |
| **Write Strategy** | Shadow Hop (Gen++) | Shadow Hop (Gen++) | Identical atomic safety guarantees. |

---

## 3. Safety Guarantees (Pico Profile)

Even in its reduced form, HN4 Pico maintains strict data integrity invariants that legacy embedded filesystems lack.

*   **No In-Place Overwrite:** All updates are Shadow Hops. Data is written to a new trajectory ($K+1$) before the pointer moves.
*   **Anchor Atomicity:** The file pointer only updates after the new data block checksum is verified.
*   **Power-Loss Rollback:** A crash during write results in the file reverting to its previous valid state, not corruption.
*   **Bounded Retry:** Allocation attempts are strictly capped (Depth = 4). No unbounded seeking or thrashing.
*   **Metadata Safety:** Failure mode is `ENOSPC`, never cross-linked chains or poisoned inodes.

---

## 4. Architectural Reductions

To fit inside an MCU, the standard HN4 layout is compressed.

### 4.1 The Pico Manifold (Linear Map)

Unlike the fractal geometry of the Standard Profile, the Pico layout is strictly linear to minimize seek latency on slow media.

```text
[ SECTOR 0 ]
+-----------------------------------+
| SUPERBLOCK (512 Bytes)            |  <-- Single Anchor Point (No Replicas)
+-----------------------------------+
| CORTEX (D0)                       |  <-- Fixed Size (e.g., 64 Slots)
| [Anchor Table]                    |
+-----------------------------------+
| ARMORED BITMAP (L3)               |  <-- Tiny Bitmap (No L2 Tree)
+-----------------------------------+
| FLUX MANIFOLD (D1)                |
| [Data Region]                     |
|                                   |
| ... (Bulk Storage) ...            |
|                                   |
+-----------------------------------+
```

### 4.2 Alignment Constraints

| Constraint | Requirement | Rationale |
| :--- | :--- | :--- |
| **Anchor Alignment** | 512-byte Sector | Atomic sector updates on SD/Floppy. |
| **Data Block** | $512\text{B} \times 2^M$ | Aligns with physical media erase/write units. |
| **Write Atomicity** | Device Sector | The driver assumes `mcu_write_block` commits the full sector. |

---

## 5. The "Micro-Cortex" (Memory Optimization)

Standard HN4 caches the entire Anchor Table. Pico uses a **Swap-Window Strategy**.

### 5.1 On-Demand Metadata
The MCU allocates a single 512-byte buffer for metadata operations.
*   **Lookup:** `open("log.txt")` reads *only* the specific 512-byte sector of the Cortex containing the target hash.
*   **Modification:** Updates happen in this buffer and flush immediately.

### 5.2 RAM Budget Breakdown (< 4KB)

| Component | Size | Notes |
| :--- | :---: | :--- |
| **Metadata Window** | 512 Bytes | Buffer for reading/modifying Anchors. |
| **Bitmap Cache** | 1024 Bytes | Tracks ~8MB of storage (or paged for larger). |
| **Staging Buffer** | 2048 Bytes | Data read/write scratchpad. |
| **Stack/Control** | 512 Bytes | Function call overhead & struct state. |
| **TOTAL** | **4096 Bytes** | **Fits in almost any MCU.** |

---

## 6. The 32-Bit Limit

Pico Profile changes the fundamental integer width to support 32-bit MCUs natively.

*   **Address Space:** 32-bit LBA (Max Volume: 2TB @ 512B sectors).
*   **Timestamps:** 32-bit Seconds (Unix Epoch).
*   **IDs:** 64-bit truncated UUIDs.

**Data Structure Compression (64-Byte Anchor):**

| Offset | Field | Bits | Description |
| :--- | :--- | :--- | :--- |
| **0x00** | `seed_id` | 64 | Truncated Physics ID. |
| **0x08** | `gravity_center` | 32 | 32-bit LBA Pointer. |
| **0x0C** | `mass` | 32 | File Size (Max 4GB). |
| **0x10** | `flags` | 32 | Simplified capability mask. |
| **0x14** | `orbit_vector` | 32 | $V$ Stride. |
| **0x18** | `fractal_scale` | 8 | $M$ (0..8). |
| **0x19** | `write_gen` | 8 | Wrap-around counter. |
| **0x1A** | `checksum` | 16 | CRC16 (CCITT). *Note: Bursts > 16 bits may pass.* |
| **0x1C** | `name` | 36 bytes | Inline Filename (Null terminated). |

---

## 7. Media-Specific Implementations

### 7.1 Legacy Floppy Disk (1.44 MB)
*   **Why:** Unlike FAT12, HN4 never leaves a filesystem "dirty" if ejected during write.
*   **Config:** $V=1$ (Sequential). $K=0$ preference minimizes head thrashing.

### 7.2 SPI NOR Flash (Embedded)
*   **Behavior:** HN4 treats NOR Flash as a block device.
*   **Erase:** Because Shadow Hops always write to *new* addresses, HN4 naturally avoids read-modify-write cycles on dirty pages.
*   **Wear Distribution:** While not a formal FTL, the randomized placement of Ballistic Allocation provides **opportunistic wear distribution**, preventing hot-spots better than static FAT tables.

### 7.3 EEPROM (Config)
*   **Atomic Config:** Saving settings is transactional. Power loss leaves the old config valid. No "half-written" Wi-Fi credentials.

---

## 8. The Micro-HAL (Hardware Abstraction)

Porting requires implementing just 3 blocking functions.

```c
/* 1. Block Read: Reads 'count' sectors. */
int mcu_read_block(uint32_t lba, uint8_t* buf, uint32_t count);

/* 2. Block Write: MUST BLOCK until physical persistence. */
int mcu_write_block(uint32_t lba, const uint8_t* buf, uint32_t count);

/* 3. Time: Monotonic milliseconds. */
uint32_t mcu_get_time_ms(void);
```

**Heap-Free:** The entire stack runs on static buffers. `malloc` is never called.

---

## 9. Performance & Behavior

### 9.1 Power-Loss Safety
Scenario: Power cut exactly during a file write.

**FAT12/16:** FAT Table updated $\to$ Data not written $\to$ **Cross-linked Chains / Corruption.** `CHKDSK` required.
**HN4 Pico:** Anchor points to $K=0$. New Data written to $K=1$. **Crash.** Anchor still points to $K=0$.
**Result:** **Atomic Rollback.** The file reverts to its previous valid state instantly.

### 9.2 Read Path Identity Validation
Every data block carries a **Well-ID** in its header. `hn4_read` verifies this ID against the Anchor's `seed_id` before returning data. This prevents returning stale or garbage data in the event of a bitmap sync error.

---

## 10. When To Use (and NOT Use) Pico

| **Use HN4 Pico When...** | **Do NOT Use HN4 Pico When...** |
| :--- | :--- |
| MCU RAM is < 32KB. | Volume size > 2TB. |
| Hard power cuts are frequent/expected. | Metadata redundancy (RAID/Mirroring) is mandatory. |
| Files are small/simple (Logs, Configs, Assets). | Workloads require complex journaling or ACLs. |
| Predictable, deterministic timing is critical. | NAND requires a full-scale, wear-leveling FTL. |

---

## 11. Validation Matrix (Embedded)

Verification plan for certification on new hardware:

1.  **Sudden Power Loss:** Cut power during 100% write load. Verify no mount errors on reboot.
2.  **Brown-out:** Slow voltage decay during writes.
3.  **Reset Storm:** Trigger MCU watchdog reset repeatedly during heavy IO.
4.  **Bit-Flip Injection:** Randomly flip bits in Superblock and Anchor Table. Verify error codes.
5.  **Full-Disk Behavior:** Fill volume to 100%. Verify `ENOSPC` is returned cleanly without corruption.
6.  **Fragmentation Hammer:** Write/Delete 10,000 small files. Measure allocation latency drift.

---

## 12. Conclusion

The **Pico Profile** proves that the Ballistic Method is universal. By stripping away the enterprise features but keeping the mathematical core ($LBA = f(x)$), we bring **Server-Grade Data Integrity** to the **$0.50 Microcontroller**. It is a crash-resilient alternative to FAT, designed to eliminate cross-linked chain corruption forever.
