# THE HELIX PROTOCOL
### *Autonomous Integrity & Self-Healing Subsystem*

**Status:** Implementation Standard v5.5
**Module:** `hn4_repair.c`
**Role:** Detection, Correction, and Isolation of Faults

---

## 1. Executive Summary

Traditional file systems treat data corruption as an exceptional event requiring offline intervention (e.g., `fsck`). HN4's **Helix Protocol** treats corruption as a statistical inevitability of physical storage media. It is an inline, zero-downtime integrity engine that reacts to I/O events in real-time.

1.  **Reactive Repair:** If a read operation detects a checksum mismatch, Helix attempts to heal the data transparently before returning it to the application.
2.  **Proactive Relocation:** If a write operation fails verification, Helix relocates the data trajectory to fresh media blocks.
3.  **Environmental Safety:** It monitors hardware telemetry (e.g., temperature) and CPU integrity to prevent the host system from inadvertently damaging the storage.

---

## 2. The CPU Sanity Interlock (The First Gate)

Before the system attempts to "fix" data, it must verify the integrity of the host processor. A bit-flip in the CPU's Arithmetic Logic Unit (ALU) or L1 Cache could cause the driver to "repair" valid data with garbage.

**The Algorithm (`_verify_cpu_sanity`):**
Upon detection of a data error, and prior to any corrective write command:
1.  **Load Vector:** Load a static byte sequence `123456789`.
2.  **Compute:** Calculate CRC32C checksum.
3.  **Compare:** Assert result equals the known constant `0xCBF43926`.
4.  **Verdict:**
    *   **Match:** CPU is sane. The fault lies with the storage media. Proceed with repair.
    *   **Mismatch:** CPU is compromised. **KERNEL PANIC IMMEDIATELY.** Do not touch the disk.

```text
      [ IO EVENT ]
           |
           v
   +----------------+      NO       +-------------+
   | CRC32 MATCH?   |-------------->|  KERNEL     |
   | (ALU CHECK)    |               |  PANIC      |
   +----------------+               +-------------+
           | YES                           ^
           v                               |
   +----------------+               (Prevents Data
   | EXECUTE HELIX  |                Shredding)
   | REPAIR ROUTINE |
   +----------------+
```

### 2.1 The Quarantine Policy
A CPU Sanity Failure is non-recoverable.
1.  **Panic:** The driver halts the kernel to prevent logical corruption propagation.
2.  **State Preservation:** The driver does *not* attempt to update the Superblock or mark the volume "Clean". This forces a full integrity check on the next boot.

---

## 3. Reactive Healing: The Read-Path Loop

When `hn4_read_block()` detects a checksum mismatch, it triggers the **Hysteresis Loop** rather than immediately returning an I/O error.

### Phase 1: Transient Defense (Hysteresis)
Storage controllers often exhibit "Transient Bitflips" due to voltage sag or noise.
1.  **Pause:** The driver sleeps for **1000µs** (`HN4_DOUBLE_READ_DELAY_US`).
2.  **Retry:** It re-issues the read command.
3.  **Result:**
    *   **Success:** The data is valid. It was a transient glitch.
    *   **Action:** **Refresh Write.** The driver writes the valid data back to the same LBA to refresh the NAND charge levels.

### Phase 2: Reconstruction & Scrub
If the retry fails, the block is considered corrupted ("Rotten").
1.  **Fetch:** The driver retrieves the data from a redundant source (Mirror or Parity Block).
2.  **Scrub:** The driver attempts to write the *corrected* data back to the *original* LBA (`hn4_repair_block`).
3.  **Verify:** Read back the scrubbed block.
    *   **Pass:** Sector repaired.
    *   **Fail:** Physical Medium Failure. Trigger **Toxic Relocation**.

---

## 4. Proactive Immunity: Write Relocation

When a write operation fails (Hardware Error) or fails Read-After-Write verification, Helix performs **Trajectory Mutation** (Relocation).

### The Relocation Logic
1.  **Lock:** Acquire exclusive access to the Anchor.
2.  **Mark:** Mark the current target LBA as **TOXIC** in the Quality Mask.
3.  **Calculate:** Compute the next orbital trajectory ($k+1$).
4.  **Write:** Commit data to the new LBA.
5.  **Update:** Update the Anchor metadata (`gravity_center` or `orbit_vector`).

**Guarantee:** The application receives a successful write completion. The data is durable at a new location, and the physical defect is permanently mapped out of the allocation pool.

---

## 5. Toxic Management (Fault Isolation)

HN4 tracks bad blocks using the **Void Bitmap** and **Quality Mask**, eliminating the need for a separate "Bad Block Table".

### 5.1 The Bitmap Override
Normally, the Void Bitmap indicates "Occupied" (1) or "Free" (0).
For a bad block, Helix marks it as **Occupied (1)**.
*   **Effect:** The Allocator will never attempt to use this block again, as it appears to be in use.

### 5.2 The Quality Mask (Silicon Cartography)
Parallel to the Bitmap is the 2-bit **Quality Mask**.
*   **00 (TOXIC):** Dead/Unsafe. Do not touch.
*   **01 (BRONZE):** Slow/Degraded. Use for transient data only.
*   **10 (SILVER):** Standard.
*   **11 (GOLD):** High-Performance/Reliability (Metadata tier).

**The Marking:**
When Helix condemns a block:
1.  **Bitmap:** Set to `1` (Occupied).
2.  **Q-Mask:** Set to `00` (Toxic).
3.  **Result:** The block is logically isolated from the filesystem.

---

## 6. The Scrubber (Background Maintenance)

The Scrubber runs during idle periods to verify the integrity of cold data.

### 6.1 Zero-Skip Optimization
Unlike tree-walking scrubbers, the HN4 Scrubber scans the **Linear Address Space** ($O(N)$) with a significant optimization.

1.  **Fetch Word:** Read 64 bits from the Void Bitmap (RAM).
2.  **Check:** Is the word `0` (Empty)?
    *   **YES:** Skip 64 blocks (Do not issue disk reads).
    *   **NO:** Iterate bits. If `1`, read and verify the corresponding LBA.
3.  **Efficiency:** Scrubbing empty space costs **Zero IOPS**.

### 6.2 Thermal Throttling
Scrubbing generates heat. Helix monitors drive temperature via the HAL.
*   **Check:** Every 256 blocks.
*   **Sensor:** Query `hn4_hal_get_temperature()`.
*   **Threshold:** If Temp > 75°C:
    *   **Action:** Suspend scrubbing for 5000ms.
    *   **Log:** "Thermal Throttle engaged."

### 6.3 Tiered Prioritization
The Scrubber uses the **Quality Mask** to prioritize workload.
*   **GOLD (Metadata):** Scrubbed aggressively.
*   **SILVER (User Data):** Standard rotation.
*   **BRONZE (Archive):** Lazy scrub (low priority).
*   **TOXIC:** Skipped entirely.

---

## 7. The Triage Log (Telemetry)

Helix records incidents in a circular memory buffer (`_triage_ring`) rather than flooding the kernel log.

**Structure:**
*   `timestamp`: Nanoseconds.
*   `lba`: The physical location.
*   `error_type`: `ROT` (Read Error), `WRITE` (Write Error), `SYNC` (Metadata Error).
*   `action`: `HEALED`, `RELOCATED`, `PANIC`.

**Persistence:** If the volume is mounted Read/Write, the Triage Log is flushed to the Superblock Journal area periodically for diagnostic analysis.

---

## 8. Critical Safety Guarantees

### 8.1 The "Do No Harm" Principle
Helix will **NEVER** overwrite a block on disk unless:
1.  The CPU Sanity Check passes.
2.  The incoming (repaired) data validates against its own checksum.
3.  The target LBA belongs to the file in question (Identity Check).

### 8.2 The Panic Cord
If Helix encounters an unrecoverable error (e.g., Metadata Corruption in the Cortex, or DED in the RAM Bitmap):
1.  **Action:** Sets `HN4_VOL_PANIC` in the Superblock state flags.
2.  **Effect:** The filesystem instantly remounts **Read-Only**.
3.  **Rationale:** Prevents automated logic from thrashing a failing drive.

### 8.3 Concurrency & Ordering
In multi-processing environments, Helix enforces strict ordering:
1.  **Sanity Fence:** The CPU Sanity Check uses a `memory_order_acquire` fence to ensure validation completes before any write instructions are pipelined.
2.  **Global Panic Visibility:** The `state_flags` are atomic; a panic set by one core is instantly visible to all others via cache coherency.
3.  **Serialized Repair:** Repair actions on a specific LBA are serialized using hashed spinlocks to prevent race conditions during relocation.
