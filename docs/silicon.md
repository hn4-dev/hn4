# HN4 ARCHITECTURE: SILICON CARTOGRAPHY
### *Quality Masking Subsystem*

**Status:** Implementation Standard v6.0
**Module:** `hn4_cartography.c`
**Role:** Physical Substrate Abstraction & Wear Management

---

## 1. Abstract: The Spectrum Model

Traditional filesystems treat storage media as binary: a block is either **Valid** or **Bad**. If a sector exhibits high latency or intermittent read failures, the filesystem relies entirely on hardware ECC. If ECC fails, the OS receives an IO error.

HN4 implements **Silicon Cartography**. This subsystem rejects the binary model in favor of a **Reliability Spectrum**. It assumes physical NAND/media quality is non-uniform due to manufacturing variance, wear leveling, or thermal stress.

Instead of hiding these variances, HN4 surveys the physical substrate to generate a **Quality Mask (Q-Mask)**. This allows the allocator to route data based on the specific durability requirements of the write operation.

---

## 2. Data Structure: The Q-Mask

Parallel to the standard 1-bit Allocation Bitmap, HN4 maintains a 2-bit **Quality Mask**.

**Storage Location:** Contiguous block immediately following the Void Bitmap.
**Memory State:** Loaded into pinned RAM during `hn4_mount`.

### 2.1 The Four Tiers

| Bits | Designation | Latency Threshold | Reliability | Allocation Policy |
| :--- | :--- | :--- | :--- | :--- |
| `11` | **GOLD** | $< 150 \mu s$ | Maximum | **Critical Only.** Superblocks, Anchors, Tethers. |
| `10` | **SILVER**| Standard | Nominal | **Default.** Standard user data, binaries, text. |
| `01` | **BRONZE**| $> 5 ms$ | Degraded | **Transient.** `HINT_LUDIC` (Game Assets), Swap, Cache. |
| `00` | **TOXIC** | Fail | Unsafe | **BANNED.** Mathematically excluded from trajectory. |

---

## 3. Initialization: The Format-Time Survey

During volume creation (`hn4_format`), the driver performs a physical characterization of the media before writing filesystem headers. This is handled by `_init_bitmap`.

### 3.1 Latency Profiling
1.  **Pattern Write:** The driver writes a test pattern (`0x5A`) to distributed sectors.
2.  **Timed Read:** The driver reads the pattern back, measuring nanosecond response time.
3.  **Binning:**
    *   **Top 5% (Fastest):** Marked **GOLD**. This isolates SLC cache regions on generic SSDs.
    *   **Bottom 10% (Slowest):** Marked **BRONZE**. Identifies QLC regions or worn cells.
    *   **Write Failures:** Marked **TOXIC**.

**Result:** The filesystem topology is physically aligned with the specific performance characteristics of the hardware instance.

---

## 4. Runtime Logic: The Quality-Aware Allocator

The **Void Engine** (Allocator) references the Q-Mask during write operations (`_check_quality_tier` in `hn4_allocator.c`).

### 4.1 Scenario A: High-Integrity Write
*   **Context:** Database commit or System Config.
*   **Flag:** `HINT_ATOMIC`.
*   **Logic:**
    1.  Allocator calculates Trajectory $T_1$.
    2.  Check Q-Mask at $T_1$.
    3.  **Result:** `01` (BRONZE).
    4.  **Decision:** **REJECT**. The data is too critical for degraded storage.
    5.  **Action:** Calculate Shadow Hop ($T_2$) until a SILVER or GOLD slot is found.

### 4.2 Scenario B: Bulk Asset Write
*   **Context:** Game installation or Texture streaming.
*   **Flag:** `HINT_LUDIC`.
*   **Logic:**
    1.  Allocator calculates Trajectory $T_1$.
    2.  Check Q-Mask at $T_1$.
    3.  **Result:** `01` (BRONZE).
    4.  **Decision:** **ACCEPT**. Read-only assets tolerate higher latency.
    5.  **Benefit:** Utilizes storage capacity that purely safe systems would discard or leave idle.

---

## 5. Self-Healing: Dynamic Degradation

HN4 manages silicon decay in real-time. If a "Silver" block exhibits errors during operation, the system downgrades it rather than failing.

### 5.1 The Demotion Workflow
1.  **Detection:** `hn4_read` detects a CRC mismatch or IO timeout on Block $X$.
2.  **Bitwise Downgrade:** The Q-Mask is updated via an atomic `AND` operation.
    *   `10` (Silver) `& 01` $\rightarrow$ `00` (Toxic) *[Severe Fail]*
    *   `10` (Silver) $\rightarrow$ `01` (Bronze) *[Latency Fail]*
3.  **Evacuation:**
    *   If Block $X$ contained Metadata, it is immediately rewritten to a GOLD vector.
    *   If Block $X$ contained User Data, it is flagged for the Scavenger to move during idle time.
4.  **Toxic Lock:** Once a block reaches `00` (TOXIC), the Void Engine treats it as a permanent gravity well. No future write trajectory will resolve to this address.

---

## 6. Technical Implications

### 6.1 Fault Isolation
The architecture allows HN4 to function on compromised media. A drive with 20% bad sectors remains usable; the `TOXIC` mask simply routes all IO to the remaining 80% valid surface area.

### 6.2 Radiation Hardening
In environments subject to Single Event Upsets (space/high-altitude), physical memory pages often degrade individually. Dynamic Degradation allows the OS to isolate damaged pages without a full system crash.

### 6.3 Performance Tiering
By forcibly mapping metadata (Anchors) to the fastest physical NAND pages (GOLD), filesystem overhead (stat/open) remains minimized, even as the drive approaches capacity or end-of-life write endurance.