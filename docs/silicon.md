# HN4: Silicon Cartography (Quality Masking)

> **"Hardware lies. Physics doesn't."**

This document details the **Silicon Cartography** subsystem, a revolutionary feature of the HYDRA-NEXUS 4 (HN4) file system.

Instead of trusting the SSD controller or RAM manufacturer to hide defects, HN4 actively **surveys** the silicon substrate, builds a **Quality Map (Q-Mask)**, and dynamically routes data based on the reliability of each physical sector.

This transforms HN4 from a passive driver into an **Adaptive Storage Hypervisor**.

---

### 1. The Core Philosophy

Legacy filesystems (Ext4, NTFS) assume a binary state for storage: **Working** or **Broken**. If a sector is slow or flaky, the filesystem waits for the hardware ECC to fix it. If that fails, the filesystem panics.

HN4 assumes hardware is a **Spectrum**:
*   Some blocks are fast (SLC Cache).
*   Some blocks are slow (QLC / Worn out).
*   Some blocks are dying (High Bit Error Rate).

We map this spectrum and use it.

---

### 2. The Data Structure: The Q-Mask

Parallel to the standard Allocation Bitmap (1-bit), HN4 maintains a **Quality Mask** (2-bits per block).

| Bits | Grade | Latency | Retention | Usage Policy |
| :--- | :--- | :--- | :--- | :--- |
| `11` | **GOLD** | $< 150 \mu s$ | Perfect | **Metadata Only.** Anchors, Superblocks, Tethers. |
| `10` | **SILVER**| Standard | Good | **Default.** User Data, Text, Source Code. |
| `01` | **BRONZE**| $> 5 ms$ | Corrected | **Transient.** Game Assets, Temp Files, Swap. |
| `00` | **TOXIC** | Fail | Fail | **BANNED.** Mathematically masked out. |

**Location:** Stored on disk immediately after the Void Bitmap region. Loaded into RAM on mount.

---

### 3. Calibration: The Format-Time Survey

When you run `hn4_format`, the driver does not just write headers. It performs a **Surface Scan Benchmark** (implemented in `_init_bitmap` inside `hn4_format.c`).

1.  **Latency Map:** The driver writes a pattern (`0x5A`) and reads it back, measuring nanosecond response time.
2.  **Binning Logic:**
    *   Fastest 5% of blocks $\to$ **GOLD**.
    *   Slowest 10% of blocks $\to$ **BRONZE**.
    *   I/O Errors $\to$ **TOXIC**.
3.  **Result:** The file system is "born" knowing exactly where the high-performance NAND dies are located physically on the chip.

---

### 4. Runtime Logic: The Adaptive Allocator

The **Void Engine (Allocator)** is now Quality-Aware (`_check_quality_tier` in `hn4_allocator.c`).

**Scenario A: Saving a Database (`HINT_ATOMIC`)**
1.  Allocator calculates trajectory $T_1$.
2.  Checks Q-Mask at $T_1$.
3.  Result: **BRONZE**.
4.  **Action:** REJECT. The database demands reliability. The allocator calculates $T_2$ (Shadow Hop) until it finds **SILVER** or **GOLD**.

**Scenario B: Installing a 100GB Game (`HINT_LUDIC`)**
1.  Allocator calculates trajectory $T_1$.
2.  Checks Q-Mask.
3.  Result: **BRONZE**.
4.  **Action:** ACCEPT. Game textures are read-only and can tolerate millisecond latency. This utilizes storage that would otherwise be wasted.

---

### 5. Self-Healing: Dynamic Degradation

What happens when a "Silver" block starts failing years later?

1.  **Detection:** The **Auto-Medic** (Read Repair) detects a CRC failure or a timeout on Block $X$.
2.  **Demotion:**
    *   The Driver performs an **Atomic Bitwise AND** on the Q-Mask to downgrade Block $X$ from `10` (Silver) to `01` (Bronze).
3.  **Evacuation:**
    *   If the data in Block $X$ was Critical (Metadata), it is immediately moved to a Gold block.
    *   The old block is freed (but now marked Bronze).
4.  **Toxic Lock:**
    *   If a block fails verify-after-write, it is marked `00` (Toxic).
    *   The Allocator treats Toxic blocks as "Occupied Gravity Wells." No future file can ever land there.

---

### 6. Why This Matters

This architecture allows HN4 to:
*   **Run on "Garbage" Hardware:** You can format a dying SD card with 20% bad sectors, and HN4 will simply map around them and use the remaining 80% reliably.
*   **Survive Radiation:** In space/nuclear environments where bitflips are common, HN4 dynamically isolates damaged RAM/Flash pages without crashing the OS.
*   **Maximize NVMe Speed:** By putting Metadata on the fastest physical NAND pages (GOLD), directory lookups and file opens remain instantaneous even as the drive fills up.

**HN4 doesn't just store data. It manages the physics of the medium.**