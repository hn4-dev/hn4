# HN4 ARCHITECTURE: SILICON CARTOGRAPHY
### *Quality Masking Subsystem*

**Status:** Implementation Standard v6.0
**Context:** Integrated into `hn4_format.c` and `hn4_repair.c`
**Role:** Physical Substrate Abstraction & Wear Management

---

## 1. Abstract: The Spectrum Model

Traditional filesystems (e.g., Ext4, NTFS) treat storage media as binary: a block is either **Valid** or **Bad**. If a sector exhibits high latency or intermittent read failures, the filesystem relies entirely on hardware ECC/FTL. If hardware correction fails, the OS receives an IO error, and the block is marked bad only after catastrophic failure.

HN4 implements **Silicon Cartography**. This subsystem rejects the binary model in favor of a **Reliability Spectrum**. It acknowledges that physical NAND/media quality is non-uniform due to manufacturing variance, wear leveling, or thermal stress.

Instead of hiding these variances, HN4 surveys the physical substrate to generate a **Quality Mask (Q-Mask)**. This allows the allocator to route data based on the specific durability requirements of the write operation, matching payload criticality to physical reliability.

---

## 2. Data Structure: The Q-Mask

Parallel to the standard 1-bit Allocation Bitmap, HN4 maintains a 2-bit **Quality Mask** in the metadata region.

**Storage Location:** Contiguous region immediately following the Allocation Bitmap (`sb.lba_qmask_start`).
**Memory State:** Loaded into pinned kernel memory during `hn4_mount`.

### 2.1 The Four Tiers

The 2-bit value represents a reliability classification for each physical block.

| Bits | Designation | Latency Threshold | Reliability | Allocation Policy |
| :--- | :--- | :--- | :--- | :--- |
| `11` | **GOLD** | $< 150 \mu s$ | Maximum | **Critical Only.** Superblocks, Anchors, Tethers. Maps to SLC cache regions. |
| `10` | **SILVER**| Standard | Nominal | **Default.** Standard user data, binaries, text. |
| `01` | **BRONZE**| $> 5 ms$ | Degraded | **Transient.** `HINT_LUDIC` (Game Assets), Swap, Cache. Maps to QLC/Worn regions. |
| `00` | **TOXIC** | Fail | Unsafe | **BANNED.** Mathematically excluded from write trajectory. |

---

## 3. Initialization: The Format-Time Survey

During volume creation (`hn4_format.c`), the driver performs a physical characterization of the media before writing filesystem headers. This logic is encapsulated in `_survey_silicon_cartography`.

### 3.1 Latency Profiling
1.  **Pattern Write:** The driver writes a deterministic test pattern (e.g., `0x5A` or `0xAA`) to distributed sectors across the LBA range.
2.  **Timed Read:** The driver reads the pattern back, measuring nanosecond response time via the HAL.
3.  **Binning:**
    *   **Top 5% (Fastest):** Marked **GOLD**. This effectively isolates SLC cache regions on generic SSDs without firmware API support.
    *   **Bottom 10% (Slowest):** Marked **BRONZE**. Identifies QLC regions or worn cells.
    *   **Write Failures:** Marked **TOXIC** immediately.

**Result:** The filesystem topology is physically aligned with the specific performance characteristics of the hardware instance at Genesis.

---

## 4. Runtime Logic: The Quality-Aware Allocator

The **Void Engine** (Allocator) references the Q-Mask during write operations (`_check_quality_compliance` in `hn4_allocator.c`).

### 4.1 Scenario A: High-Integrity Write
*   **Context:** Database commit (`WAL`), System Configuration, or Anchor Update.
*   **Flag:** `HN4_ALLOC_METADATA` or `HN4_VOL_STATIC`.
*   **Logic:**
    1.  Allocator calculates Trajectory $T_1$.
    2.  Check Q-Mask at $T_1$.
    3.  **Result:** `01` (BRONZE).
    4.  **Decision:** **REJECT**. The data integrity requirement exceeds the physical reliability of the block.
    5.  **Action:** Calculate Shadow Hop ($T_2$) until a SILVER or GOLD slot is found.

### 4.2 Scenario B: Bulk Asset Write
*   **Context:** Game installation, Texture streaming, or Temp files.
*   **Flag:** `HN4_ALLOC_LUDIC`.
*   **Logic:**
    1.  Allocator calculates Trajectory $T_1$.
    2.  Check Q-Mask at $T_1$.
    3.  **Result:** `01` (BRONZE).
    4.  **Decision:** **ACCEPT**. Read-only assets tolerate higher latency and lower retention guarantees.
    5.  **Benefit:** Utilizes storage capacity that purely safe systems would discard or leave idle, maximizing total effective capacity.

---

## 5. Self-Healing: Dynamic Degradation

HN4 manages silicon decay in real-time via the Auto-Medic (`hn4_repair.c`). If a "Silver" block exhibits errors during operation, the system downgrades it rather than failing the IO.

### 5.1 The Demotion Workflow
1.  **Detection:** `hn4_read` detects a CRC mismatch or IO timeout on Block $X$.
2.  **Repair Attempt:** The Auto-Medic attempts to rewrite the data (`hn4_repair_block`).
3.  **Bitwise Downgrade:** The Q-Mask is updated via an atomic `CAS` operation.
    *   **Repair Success:** `10` (Silver) $\rightarrow$ `01` (Bronze). The block is healed but marked as suspect.
    *   **Repair Failure:** `10` (Silver) $\rightarrow$ `00` (Toxic). The block is physically dead.
4.  **Evacuation:**
    *   If Block $X$ contained Metadata, it is immediately rewritten to a new GOLD vector via Atomic Relocation.
    *   If Block $X$ contained User Data, it is flagged for the Scavenger to move during idle time.
5.  **Toxic Lock:** Once a block reaches `00` (TOXIC), the Void Engine treats it as a permanent gravity well. No future write trajectory will resolve to this address.

---

## 6. Technical Implications

### 6.1 Fault Isolation
The architecture allows HN4 to function on compromised media. A drive with 20% bad sectors remains usable; the `TOXIC` mask simply routes all IO to the remaining 80% valid surface area. This contrasts with traditional RAID/Filesystems which often fail the entire drive upon reaching a bad block threshold.

### 6.2 Radiation Hardening
In environments subject to Single Event Upsets (space/high-altitude), physical memory pages often degrade individually. Dynamic Degradation allows the OS to isolate damaged pages without a full system crash or reboot.

### 6.3 Performance Tiering
By forcibly mapping metadata (Anchors) to the fastest physical NAND pages (GOLD), filesystem overhead operations (stat/open/link) remain performant, even as the drive approaches capacity or end-of-life write endurance.
