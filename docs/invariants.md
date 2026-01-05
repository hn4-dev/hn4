# HN4: Architectural Invariants & Consistency Model

**Document ID:** HN4-ARCH-INV
**Spec Version:** 4.2 (Reference Standard)
**Implementation:** Bare Metal / Kernel
**Status:** **IMMUTABLE**

This document defines the mathematical laws and state transitions that the Hydra-Nexus 4 driver must enforce. **Violation of any invariant defined herein constitutes a Critical Driver Failure (`HN4_ERR_INTERNAL_FAULT`).**

---

## 1. THE PHYSICS INVARIANTS (Immutable Laws)

These properties rely on the Ballistic Math (Spec §6) and must hold true for every IO operation.

### 1.1. Trajectory Determinism
*   **Law:** For any given tuple $\{G, V, M, N\}$ (Gravity, Vector, Scale, Sequence) on a Volume with capacity $C$, the resulting Physical LBA is constant.
*   **Invariant:** The file system does not "store" the location of Block $N$. It **calculates** it.
*   **Constraint:** The equation `LBA = (G + N*V*2^M) % (C_flux)` is absolute.

### 1.2. The Quantum Exclusion Principle (Spec §22.1)
*   **Law:** Two active Anchors **MUST NOT** share the same Gravity Center ($G$) unless they are Snapshots (Frozen).
*   **Invariant:** Block-level deduplication is physically forbidden.
*   **Reasoning:** HN4 uses a 1-bit Void Bitmap. It lacks Reference Counting. Shared mutable blocks would lead to corruption upon deletion.

### 1.3. The Coprimality Invariant (Spec §18.2)
*   **Law:** For any allocated file, the Orbit Vector $V$ and the Effective Capacity $\Phi_M$ must be coprime: $\gcd(V, \Phi_M) = 1$.
*   **Invariant:** This guarantees a file generates a Full Cycle Group, visiting every block in the address space exactly once before repeating.
*   **Enforcement:** The Void Engine rejects any $V$ that violates this during Genesis.

### 1.4. The Non-Resonance of Time (Spec §15.6)
*   **Law:** `Anchor.seed_id` (UUID v7) is time-ordered.
*   **Invariant:** `ID_A < ID_B` implies `Creation_Time(A) <= Creation_Time(B)`.
*   **Usage:** Allows the Cortex to be sorted/searched by time without an auxiliary index.

---

## 2. THE STATE TRANSITION MATRIX (Consistency)

HN4 utilizes **Shadow Hops** (Spec §6.3) and **Epoch Rings** (Spec §2.3) instead of journaling. The volume moves between valid states atomically.

### 2.1. The Atomic Write Lifecycle
Guarantees consistency without a Write-Ahead Log.

| Step | State | Action | Failure Result |
| :--- | :--- | :--- | :--- |
| **1** | **Genesis** | Allocator finds free Trajectory $T_{new}$ in RAM Bitmap. | RAM lost. Disk untouched. Safe. |
| **2** | **Materialization** | Data written to $T_{new}$. | $T_{new}$ contains data, but no Anchor points to it. **Orphaned.** Safe. |
| **3** | **Barrier** | `NVMe Flush` / `FUA`. | Data persisted to NAND. |
| **4** | **The Hop** | Anchor updated in D0 (RAM) to point to $T_{new}$. Gen++. | Anchor in RAM differs from Disk. |
| **5** | **Commit** | Anchor Page written to D0 (Disk). | **Atomicity Point.** The file effectively "moves." |
| **6** | **Eclipse** | Old Trajectory $T_{old}$ freed/zeroed. | Irrelevant. |

### 2.2. The Epoch Invariant (Spec §2.3)
*   **Law:** The `Superblock.current_epoch_id` MUST monotonically increase.
*   **Invariant:** The `Epoch_Ring` contains the Root Checksum of the Anchor Table at time $T$.
*   **Recovery Guarantee:** If the Superblock is corrupt or "Future-dated" (due to torn write), the driver can roll back to `Epoch[N-1]` and find a mathematically consistent Cortex.

---

## 3. THREAT MODEL & DEFENSE SURFACE

How HN4 handles hostile physics.

| Threat | HN4 Mechanism | Invariant Enforced |
| :--- | :--- | :--- |
| **Bit Rot (NAND)** | **Holo-Lattice (Spec §11.1)** | `CRC32(Block) == Expected`. Read fails if mismatch. |
| **RAM Bitflip** | **Armored Bitmap (Spec §5.3)** | 64-bit Word + 8-bit ECC. 1-bit errors corrected transparently. |
| **Phantom Write** | **Generation Counter (Spec §23.2)** | `Block.Gen == Anchor.Gen`. Stale blocks ignored. |
| **Torn Write** | **Shadow Hop (Spec §6.3)** | Metadata never points to partial data. |
| **Ransomware** | **Chrono-Fencing (Spec §24.3)** | `PERM_IMMUTABLE` requires Sovereign Key to unset. |
| **Time Travel** | **The Chronicle (Spec §24.5)** | Moving `Epoch_Ptr` backwards MUST append Audit Log. |

---

## 4. BOUNDARY BEHAVIORS (Spec Compliance)

These are not "limitations" but **defined behaviors** at the edges of the operational envelope.

### 4.1. The Event Horizon (Capacity > 95%)
*   **Behavior:** The probability of finding a ballistic slot ($k \le 12$) drops.
*   **Response:** The Allocator transitions the file to **D1.5 (Horizon)** (Spec §6.4).
*   **Invariant:** System never returns `ENOSPC` while raw blocks exist.
*   **Trade-off:** Addressing shifts from $O(1)$ Math to $O(1)$ Append. Seek becomes $O(Log N)$ via Skip-List.

### 4.2. The Inertial Damper (Rotational Media)
*   **Condition:** `SB.device_type == HDD` or `PROFILE_ARCHIVE`.
*   **Behavior:**
    *   Shotgun Read (4x Parallel) is **Disabled**.
    *   Orbit Vector forced to $V=1$ (Sequential).
*   **Invariant:** HN4 never issues non-sequential seek commands to spinning rust.

### 4.3. ZNS Alignment (Zoned Namespaces)
*   **Condition:** `SB.hw_caps & ZNS`.
*   **Behavior:**
    *   Allocations must fill a Zone sequentially.
    *   `Shadow Hop` is disabled (Append Only).
*   **Invariant:** `Write_Pointer` of a Zone must never regress.

---

## 5. SILICON CARTOGRAPHY (Quality of Service)

The **Quality Mask (QMask)** is a parallel bitmap tracking silicon health (Spec §32.1).

*   **Invariant:** A file marked `VOL_ATOMIC` (Database) **MUST** reside on `Q_SILVER` or `Q_GOLD` blocks.
*   **Invariant:** A block marked `Q_TOXIC` (00) is mathematically removed from the Void Engine and will **never** be generated by a Trajectory calculation.

| Tier | Definition | Usage Rule |
| :--- | :--- | :--- |
| **GOLD** | Ultra-Low Latency | D0 Cortex, Superblocks. |
| **SILVER** | Standard Flash | User Data (D1). |
| **BRONZE** | High Latency / Retries | D2 Streams, Archive. |
| **TOXIC** | Dead Cell | **Forbidden.** |

---

## 6. VALIDATION LOGIC (The "Spot Check")

To verify these invariants in a running system:

1.  **Mount Check:**
    *   `assert(SB.magic == 0x48594452415F4E34)`
    *   `assert(SB.block_size % 512 == 0)`
2.  **Runtime Check (Helix):**
    *   On Read: `assert(Header.Well_ID == Anchor.Seed_ID)`
    *   On Read: `assert(CRC32(Payload) == Header.Data_CRC)`
3.  **Sanity Check (CPU):**
    *   Before *any* Write: `assert(CRC32("123456789") == 0xCBF43926)`
    *   *Reason:* If the CPU ALU is broken, the driver must PANIC, not corrupt the disk.