# THE HELIX PROTOCOL
### *The Immune System of the Manifold*

**STATUS:** REFERENCE STANDARD (v5.5)
**TARGET:** KERNEL / BARE METAL HAL
**SOURCE:** `hn4_repair.c`

---

## 1. The Core Concept

Traditional file systems treat data corruption as an exceptional event requiring offline intervention (`fsck`, `chkdsk`).
**HN4 treats corruption as a statistical inevitability.**

**The Helix Protocol** is an inline, zero-downtime immune system. It does not "scan" the drive in a maintenance window; it reacts biologically to I/O events.
1.  **Reactive:** If a read fails, Helix heals it instantly before returning data to the user.
2.  **Proactive:** If a write fails, Helix relocates the trajectory to fresh media.
3.  **Environmental:** It monitors temperature and CPU sanity to prevent the host from damaging the storage.

**Positioning Statement:**
*Helix is not a doctor that visits when you are sick. It is the white blood cell count in the blood stream.*

---

## 2. The CPU Sanity Interlock (The First Gate)

Before HN4 attempts to "fix" data, it must verify that the "Doctor" (The CPU) is not hallucinating. A bit-flip in the CPU's ALU or L1 Cache could cause the driver to "repair" valid data with garbage.

**The Algorithm (`_verify_cpu_sanity`):**
Every time a corruption is detected, and before any write command is issued:
1.  **Load Vector:** Load a static byte sequence `123456789`.
2.  **Compute:** Calculate CRC32C.
3.  **Compare:** Assert result equals `0xCBF43926`.
4.  **Verdict:**
    *   **Match:** CPU is sane. The disk is corrupt. Proceed with repair.
    *   **Mismatch:** CPU is compromised. **PANIC IMMEDIATELY.** Do not touch the disk.

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

### 2.1 The "Bad Silicon" Quarantine

**Constraint:** A CPU Sanity Failure is not a "retryable" event. It indicates that the Arithmetic Logic Unit (ALU) or L1 Cache is physically compromised.

**The Policy:**
1.  **Panic:** If `_verify_cpu_sanity()` fails, the driver issues a **Kernel Panic** (or `exit(1)` in userspace).
2.  **The Black Flag:** Before dying, if possible, it sets a volatile RAM flag `HN4_HOST_COMPROMISED`.
3.  **The Reboot Rule:** The volume requires a **Cold Boot**. The driver logic assumes that if the CPU lied about `1 + 1 = 2` once, it will lie about pointers next.
    *   *Implementation:* The driver does not attempt to "cleanly unmount" (write the Superblock) on a Sanity Panic. It prefers to leave the volume "Dirty" (triggering Epoch Rollback on next boot) rather than risking a corrupt Superblock write by a zombie CPU.

---

## 3. Reactive Healing: The Read-Path Loop

When `hn4_read_block()` detects a checksum mismatch, it does not immediately return `EIO`. It triggers the **Hysteresis Loop**.

### Phase 1: The Hysteresis Wait (Transient Defense)
SSD controllers and cabling often suffer from "Transient Bitflips" due to voltage sag or thermal noise.
1.  **Pause:** The driver sleeps for **1000µs** (`HN4_DOUBLE_READ_DELAY_US`).
2.  **Retry:** It issues the read command again.
3.  **Result:**
    *   **Success:** The data is valid. It was a glitch.
    *   **Action:** **Refresh Write.** The driver writes the valid data *back* to the same LBA to refresh the NAND charge levels.

### Phase 2: The Helix Reconstruction (Rot Defense)
If the retry fails, the block is "Rotten."
1.  **Fetch:** The driver retrieves the Parity Block (from Orbit $k+1$) or Mirror (from Drive B).
2.  **Reconstruct:** XOR Math restores the original payload in RAM.
3.  **Scrub:** The driver attempts to write the *corrected* data back to the *original* LBA.
4.  **Verify:** Read back the scrubbed block.
    *   **Pass:** Sector repaired.
    *   **Fail:** Physical Medium Failure. Trigger **Toxic Relocation**.

```text
READ REQUEST (LBA N)
      |
      v
[ CRC CHECK ] ----> PASS ----> [ RETURN DATA ]
      |
     FAIL
      |
      v
[ WAIT 1ms ] (Hysteresis)
      |
[ RETRY READ ] ---> PASS ----> [ REFRESH WRITE ] -> [ RETURN ]
      |
     FAIL (Hard Rot)
      |
      v
[ RECONSTRUCT ] (Parity/Mirror)
      |
[ ATTEMPT SCRUB ] (Write back to LBA N)
      |
[ VERIFY ] -------> PASS ----> [ LOG HEAL ] ------> [ RETURN ]
      |
     FAIL (Dead Cell)
      |
      v
[ MARK TOXIC ] ---> [ RELOCATE ] -> [ RETURN ]
```

---

## 4. Proactive Immunity: Write Relocation

When a write fails (Hardware Error) or fails verification (Read-After-Write), Helix performs **Trajectory Mutation**.

### The Shadow Hop Logic
1.  **Lock:** Acquire Anchor Lock.
2.  **Mark:** Mark the current target LBA as **TOXIC** (See Section 5).
3.  **Math:** Calculate the next orbital trajectory ($k+1$).
4.  **Write:** Commit data to the new LBA.
5.  **Link:** Update Anchor `gravity_center` or `orbit_vector`.

**The Guarantee:** The application sees a successful write. The latency increases slightly, but the data is durable. The physical defect is permanently mapped out of the equation.

---

## 5. Toxic Management (The Quarantine)

How does HN4 remember bad blocks without a "Bad Block Table"?
It uses the **Void Bitmap** and **Quality Mask**.

### 5.1 The Bitmap Override
Usually, the Void Bitmap indicates "Occupied" (1) or "Free" (0).
For a bad block, Helix marks it as **Occupied (1)**.
*   **Effect:** The Allocator will never try to put new data there, because it thinks the space is taken.

### 5.2 The Quality Mask (Silicon Cartography)
Parallel to the Bitmap is the 2-bit **Quality Mask**.
*   **00 (TOXIC):** Dead. Do not touch.
*   **01 (BRONZE):** Slow/Noisy. Use for archival only.
*   **10 (SILVER):** Standard.
*   **11 (GOLD):** High-speed/Low-latency (Metadata tier).

**The Marking:**
When Helix condemns a block:
1.  **Bitmap:** Set to `1` (Occupied).
2.  **QMask:** Set to `00` (Toxic).
3.  **Result:** The block is "Allocated to the Void." It is effectively electrically isolated from the filesystem logic.

---

## 6. The Scrubber (Background Maintenance)

The Scrubber runs when the system is idle. It verifies the integrity of "Cold Data."

### 6.1 Zero-Skip Optimization
Traditional scrubbers (ZFS) walk the metadata tree ($O(Tree)$).
HN4 Scrubber walks the **Linear Address Space** ($O(N)$), but with a massive optimization.

1.  **Fetch Word:** Read 64 bits from Void Bitmap (RAM).
2.  **Check:** Is the word `0` (Empty)?
    *   **YES:** Skip 64 blocks. (Do not read disk).
    *   **NO:** Iterate bits. If `1`, read corresponding LBA.
3.  **Result:** Scrubbing empty space costs **Zero IOPS**. Scrubbing sparse disks is near-instant.

### 6.2 Thermal Throttling
Scrubbing generates heat. Helix monitors the drive temperature.

**Logic:**
*   **Check:** Every 256 blocks (`HN4_SCRUB_THERMAL_CHECK_MASK`).
*   **Sensor:** Query `hn4_hal_get_temperature()`.
*   **Threshold:** If Temp > 75°C:
    *   **Action:** Sleep 5000ms.
    *   **Log:** "Thermal Throttle engaged."

```text
[ SCRUB LOOP ]
      |
      +---> [ READ BITMAP WORD ]
      |           |
      |         IS ZERO? --YES--> [ SKIP 256KB ] --+
      |           |                                |
      |          NO                                |
      |           |                                |
      |     [ CHECK TEMP ] --HIGH-> [ SLEEP ]      |
      |           |                                |
      |     [ READ LBA ]                           |
      |     [ VERIFY CRC ] --FAIL-> [ TRIGGER HELIX ]
      |           |
      +-----------+
```

### 6.3 Q-Tier Prioritization

The Scrubber uses the **Quality Mask** (Section 5.2) to prioritize its workload. Not all bits are equal.

| Tier | Name | Scrub Cadence |
| :--- | :--- | :--- |
| **11** | **GOLD** (Metadata/Kernels) | **Aggressive.** Scrubbed every cycle. |
| **10** | **SILVER** (User Data) | **Standard.** Scrubbed on rotation. |
| **01** | **BRONZE** (Archival/Blob) | **Lazy.** Scrubbed only when IO load < 5%. |
| **00** | **TOXIC** (Dead) | **Skipped.** Never read. |

**The Triage Feedback Loop:**
*   If the Triage Log shows a density of errors in Region X:
*   The Scrubber temporarily promotes Region X to **Priority Status**, treating it as "Gold" until a full pass completes without error.

---

## 7. The Triage Log (Telemetry)

Helix does not spam `dmesg`. It records incidents in a high-speed circular memory buffer (`_triage_ring`).

**Structure:**
*   `timestamp`: Nanoseconds.
*   `lba`: The physical location.
*   `error_type`: `ROT` (Read), `WRITE` (Hardware), `SYNC` (Metadata).
*   `action`: `HEALED`, `RELOCATED`, `PANIC`.

**Persistence:**
If the volume is R/W, the Triage Log is flushed to the **Superblock Journal** area periodically, allowing admins to see the "health trend" of the silicon over time.

---

## 8. Critical Safety Guarantees

### 8.1 The "Do No Harm" Principle
Helix will **NEVER** overwrite a block on disk unless:
1.  The CPU Sanity Check passes.
2.  The Incoming Data (Repaired) validates against its own Checksum.
3.  The Target LBA belongs to the file in question (Identity Check).

### 8.2 The Panic Cord
If Helix encounters an error it cannot solve (e.g., Metadata Corruption in the Cortex itself, or Bitmap Corruption in RAM):
1.  **Action:** It sets `VOL_PANIC` in the Superblock state flags.
2.  **Effect:** The File System instantly remounts **Read-Only**.
3.  **Why:** It is better to stop and force manual recovery than to let an automated process thrash a dying drive.

### 8.3 Concurrency & The Memory Barrier

In SMP (Symmetric Multi-Processing) environments, Helix enforces strict ordering:

1.  **Sanity Fence:** The CPU Sanity Check includes a `memory_order_acquire` fence. This ensures the check actually runs *before* the subsequent write instructions are pipelined.
2.  **Global Panic Visibility:**
    *   The `sb.state_flags` is an `_Atomic` variable.
    *   If Thread A detects insanity and sets `VOL_PANIC`:
    *   Thread B (running on Core 4) sees this *instantly* via cache coherency protocols before it issues its next IO.
3.  **Single-Threaded Repair:**
    *   While Read/Write paths are lock-free, **Repair Actions** on a specific LBA are serialized.
    *   The driver uses a hashed spinlock on the Anchor Address to ensure two threads don't try to "heal" the same file simultaneously (which would cause a race on the Shadow Hop trajectory).

---

## 9. Summary

The Helix Protocol turns the storage engine into a biological entity.
*   It feels pain (Read Errors).
*   It heals wounds (Refresh/Relocate).
*   It scars over dead tissue (Toxic Marking).
*   It regulates its body temperature (Throttling).

It is the reason HN4 can run on cheap, degraded NAND flash without losing data.