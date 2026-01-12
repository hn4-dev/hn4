# HN4 SCAVENGER ARCHITECTURE
### *The Entropic Custodian*

**Status:** Implementation Standard v6.0
**Module:** `hn4_scavenger.c`
**Role:** Background Optimization & Healing

---

## 1. Abstract: Non-Blocking Maintenance

Standard filesystem maintenance is historically blocking. Tools like `fsck`, `defrag`, or `ZFS Scrub` often saturate IO bandwidth or lock the drive, degrading performance for active applications.

The **HN4 Scavenger** (The Entropic Custodian) functions as a non-blocking, budget-aware micro-service. It operates strictly on available "Time Slices" (e.g., 5ms execution windows) when the drive queue is idle. This ensures that maintenance never introduces latency spikes to foreground workloads like database queries or real-time rendering.

Its primary function is **Entropy Reversal**: incrementally transitioning the filesystem from a fragmented state to a mathematically optimized state without requiring offline time.

---

## 2. Core Functions

The Scavenger operates cyclically through three specific directives.

### 2.1 The Chronophage (TTL Reaper)
**Goal:** Automated cleanup of temporary files based on Time-To-Live (TTL).

*   **Logic:**
    1.  Scans the Anchor Table (D0).
    2.  Filters for `Anchor.flags & HN4_FLAG_TTL`.
    3.  Compares `Anchor.mod_clock` against the current system time.
    4.  **Action:** If `Now > Death_Time`:
        *   Converts the Anchor to a **Tombstone**.
        *   Reclaims Blocks in the Allocation Bitmap.
        *   Emits an Audit Log entry.
*   **Operational Impact:** Eliminates the "Disk Full of Temp Files" failure state at the driver level. Applications are relieved of the requirement to run dedicated cleanup scripts.

### 2.2 The Hyper-Stitcher (Stream Optimization)
**Goal:** Elimination of $O(N)$ read penalties in Linked List structures (Horizon/Stream mode).

Sequential writes in D2 naturally form a chain: `Block 1 -> Block 2 -> Block 3`. Seeking to a high offset (e.g., Block 10,000) typically requires 10,000 iterative reads.

*   **Logic:**
    1.  Detects long block chains during idle scans.
    2.  Calculates physical addresses for stride intervals (e.g., Block 1024).
    3.  Injects a **Hyper-Link** (`hyper_strm`) into the Block 0 header, pointing directly to the stride target.
*   **Result:** Converts linear search into a **Skip-List Search** ($O(\log N)$).
*   **Use Case:** Enables instant seeking in massive media files (e.g., 50GB video) immediately after recording, regardless of physical fragmentation.

### 2.3 Orbit Tuning (Mathematical Defragmentation)
**Goal:** Restoration of $O(1)$ access times for collision-heavy files.

During high-congestion writes, files may settle into high-collision hash orbits ($k=5, 6$). This increases lookup latency.

*   **Logic:**
    1.  Identifies "Stressed" Anchors (High `k_avg`).
    2.  **Solver:** Calculates a new Prime Vector ($V'$) that maps the file to a low-collision slot within the *current* empty space map.
    3.  **Migration:**
        *   Reads file via Old Vector $V$.
        *   Writes file via New Vector $V'$.
        *   Performs an Atomic Swap of the metadata.
*   **Differentiation:** Unlike standard defragmentation, which physically consolidates blocks, Orbit Tuning re-optimizes the hashing mathematics so blocks land in naturally efficient slots.

---

## 3. The Budgeting System (Zero-Stutter)

The Scavenger is designed to be invisible to the host OS scheduler.

### 3.1 The IO Budget
*   **Input:** `hn4_scavenge_pulse(vol, ops_budget)`.
*   **Metric:** 1 OP = 4KB Read/Write.
*   **Trigger:** The OS calls this function only when the NVMe Submission Queue is empty.
*   **Constraint:** If `ops_budget` hits 0, the Scavenger saves its cursor state and returns immediately. This prevents the maintenance thread from delaying incoming read/write requests.

### 3.2 Thermal Hysteresis
Scrubbing operations generate significant controller heat.
*   **Sensor:** polls `NVMe SMART Temp` every 256 blocks.
*   **Logic:** If `Temp > 75°C`: **Sleep 5000ms.**
*   **Goal:** Prevents the Scavenger from inducing hardware thermal throttling during intensive tasks (e.g., gaming or compiling).

---

## 4. Crash Recovery (The Triage Log)

In the event of a system crash, the Scavenger acts as a forensic recovery tool on the next boot.

*   **Logic:**
    1.  Reads the **Triage Log** (Circular Buffer).
    2.  Identifies blocks marked `HN4_VOL_DIRTY` (writes that were in-flight during the crash).
    3.  Verifies checksums for these specific blocks.
        *   **Valid:** Clear Dirty flag.
        *   **Invalid:** Trigger **Helix Repair** (parity reconstruction).
*   **Benefit:** Eliminates the need for full-disk scans (`fsck`). Only blocks potentially affected by the crash are verified.

---

## 5. Summary

The Scavenger is the **Entropy Reversal Engine** for HN4. While user activity increases system entropy (writes/deletes/fragmentation), the Scavenger continuously restores mathematical order in the background, ensuring the filesystem performance characteristics improve, rather than degrade, over the drive's lifespan.