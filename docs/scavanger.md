# HN4 SCAVENGER ARCHITECTURE
### *The Entropic Custodian*

**Status:** Implementation Standard v6.0
**Module:** `hn4_scavenger.c`
**Role:** Background Optimization & Healing

---

## 1. Abstract: The Passive Gardener

In traditional filesystems, maintenance is **Blocking**.
*   `fsck` locks the drive on boot.
*   `defrag.exe` grinds the HDD for hours.
*   `ZFS Scrub` saturates IO bandwidth.

The **HN4 Scavenger** is a **Non-Blocking, Budget-Aware Micro-Service**.
It runs only when the drive queue is idle. It operates on "Time Slices" (e.g., 5ms budget), ensuring it never interrupts a Game or Database query.

Its job is to enforce **Fluid Dynamics**: slowly moving the filesystem from a state of *Chaos* (Fragmentation) to *Order* (Optimization) without user intervention.

---

## 2. Core Functions

The Scavenger has three prime directives, executed cyclically.

### 2.1 The Chronophage (TTL Reaper)
**Goal:** Cleanup temporary files without explicit delete commands.

*   **Logic:**
    1.  Scan Anchor Table (D0).
    2.  Check `Anchor.flags & HN4_FLAG_TTL`.
    3.  Check `Anchor.mod_clock` (Death Time).
    4.  **Action:** If `Now > Death_Time`:
        *   Convert Anchor to **Tombstone**.
        *   Free Blocks in Bitmap.
        *   Emit Audit Log.
*   **Impact:** Solves the "Disk Full of Temp Files" problem at the driver level. Apps don't need to run cleanup scripts.

### 2.2 The Hyper-Stitcher (Stream Optimization)
**Goal:** Fix the $O(N)$ read penalty of Linked Lists (Horizon/Stream mode).

When a file is written sequentially in D2, it forms a chain: `Block 1 -> Block 2 -> Block 3`. Seeking to Block 10,000 requires 10,000 reads.

*   **Logic:**
    1.  The Scavenger detects a long chain.
    2.  It calculates the physical location of Block 1024.
    3.  It updates Block 0's header with a **Hyper-Link** (`hyper_strm`) pointing directly to Block 1024.
*   **Result:** A linear search becomes a **Skip-List Search** ($O(\log N)$).
*   **User Experience:** Seeking in a 50GB video file becomes instant, even if the file was just recorded.

### 2.3 Orbit Tuning (Defragmentation)
**Goal:** Restore $O(1)$ access to fragmented files.

Files can end up in high collision orbits ($k=5, 6$) due to disk crowding. This hurts read latency.

*   **Logic:**
    1.  Identify "Stressed" Anchors (High `k_avg`).
    2.  **Solve:** Calculate a new Prime Vector ($V'$) that fits the file better into the *current* empty space map.
    3.  **Migrate:**
        *   Read File using Old $V$.
        *   Write File using New $V'$.
        *   Atomic Swap.
*   **Difference from Defrag:** We don't just "move blocks together." We "retune the math" so the blocks land in better slots naturally.

---

## 3. The Budgeting System (Zero-Stutter)

The Scavenger must be invisible.

### 3.1 The IO Budget
*   **Input:** `hn4_scavenge_pulse(vol, ops_budget)`.
*   **Metric:** 1 OP = 4KB Read/Write.
*   **Control:** The OS Scheduler calls this function only when the NVMe Submission Queue is empty.
*   **Hard Stop:** If `ops_budget` hits 0, the Scavenger saves its state (Cursor) and returns immediately.

### 3.2 Thermal Hysteresis
Scrubbing generates heat.
*   **Sensor:** Check `NVMe SMART Temp` every 256 blocks.
*   **Logic:** If `Temp > 75°C`: **Sleep 5000ms.**
*   **Goal:** Prevent the maintenance thread from throttling the GPU during a gaming session.

---

## 4. Crash Recovery (The Triage Log)

If the system crashes, the Scavenger acts as the forensic team.

*   **On Boot:** It reads the **Triage Log** (Circular Buffer).
*   **Analysis:**
    *   Finds blocks marked `HN4_VOL_DIRTY`.
    *   Re-scans their checksums.
    *   If valid: Clear Dirty flag.
    *   If invalid: Trigger **Helix Repair**.
*   **Benefit:** This avoids a full-disk scan. Only the blocks that were "in flight" during the crash are checked.

---

## 5. Summary

The Scavenger is the **Entropy Reversal Engine**.
While the user creates chaos (writes/deletes), the Scavenger quietly restores mathematical order in the background, ensuring the filesystem gets *faster* over time, not slower.