# THE SHADOW HOP PROTOCOL
### *Beyond Copy-on-Write: Atomic Mutation via Ballistic Trajectory*

**Status:** Implementation Standard v6.1
**Module:** `hn4_write.c`
**Metric:** Linearizable Atomicity / $O(1)$ Metadata Overhead

---

## 1. The Core Architecture

Legacy Copy-on-Write (CoW) filesystems (ZFS, Btrfs) rely on pointer trees. To modify a data block atomically, they must allocate a new block, write the data, and then bubble the pointer update up the tree to the root. This is the **"Tree Tax"**: a single 4KB data write can trigger 16KB+ of metadata writes and multiple seeking I/O operations.

**HN4 rejects pointers.** It utilizes **Ballistic Equations**.
To modify data, the engine does not update a pointer; it solves for a new trajectory coordinate.

**The Shadow Hop** is the mechanism by which data moves to a new physical location without requiring a metadata tree rewrite. It exploits the multi-variate nature of the HN4 addressing equation to create "Orbital Shells" ($k$) that serve as atomic versioning slots.

---

## 2. Mathematical Foundation

The location of any block is derived from the immutable physics parameters stored in the file's **Anchor** (Spec Section 6.1):

$$ LBA_{phys} = LBA_{base} + \left( [ G + (N \times V \times 2^M) + \Theta(k) ] \pmod{\Phi} \right) $$

*   **$G$ (Gravity Center):** Physical start index.
*   **$N$ (Sequence):** Logical block offset.
*   **$V$ (Vector):** Stride/Velocity.
*   **$M$ (Scale):** Block size exponent ($2^M$).
*   **$\Theta(k)$:** The Collision/Version offset function.

In a resting state, data resides at **$k=0$** (Ground State).
When modifying data, the engine does not overwrite $k=0$. It solves for **$k=1$** (Shadow State).

### Bounded Search Window
The reader probes $k$ within a strict window (default $k \in \{0..12\}$).
*   **Standard Operation:** Files oscillate purely between $k=0$ and $k=1$.
*   **Pathological Collision:** If all orbital slots are physically blocked (rare), the Void Engine triggers **Vector Reseeding** ($V'$) to migrate the block to a completely new trajectory.
*   **Complexity:** Lookup is effectively $O(1)$, capped at $O(k_{max})$.

---

## 3. The Shadow Hop Pipeline (Execution Path)

**Scenario:** Atomic overwrite of **Logical Block 5**.

### PHASE 1: Residency Resolution
The driver calculates the physical LBA for Block 5 at **$k=0$**.
*   It reads the header to verify `Generation == Anchor.write_gen`.
*   This identifies the "Old Data" to be eclipsed later.

### PHASE 2: The Hop (Allocation)
The driver calculates the trajectory for **$k=1$**.
*   It checks the in-memory **Void Bitmap**.
*   If $k=1$ is free, it is reserved. If occupied, it probes $k=2$.

### PHASE 3: The Write (Shadow Commit)
The driver writes the **New Data** to the Shadow Slot ($k=1$).
*   **Header Tag:** `Header.Generation` is set to `Anchor.write_gen + 1`.
*   **State:** The disk now contains Old Data (Gen 100) and New Data (Gen 101). The New Data is logically invalid because the Anchor still references Gen 100.

### PHASE 4: The Hardware Barrier (FUA)
The driver issues a **Force Unit Access (FUA)** or `NVMe Flush`.
*   **Invariant:** The Anchor Update (Phase 5) is *never* issued until the storage controller acknowledges the Shadow Write is durable on NAND.

### PHASE 5: The Anchor Update (Linearization Point)
The driver updates the **Anchor** in RAM and flushes it to disk.
*   **Mutation:** `Anchor.write_gen` increments to **101**.
*   **The Switch:** This single 128-byte write atomically invalidates the old block ($k=0$) and validates the new block ($k=1$) for all subsequent readers.

### PHASE 6: The Eclipse (Deallocation)
Once the Anchor is secure:
*   The driver issues an internal **Eclipse** command for the old $k=0$ location.
*   **Action:** Updates in-memory bitmap (Clear Bit) and optionally issues TRIM/Discard.
*   **Failure Mode:** If power fails here, the old block remains allocated but "stale" (Gen 100 < Anchor 101). It is effectively garbage, cleaned up later by the Scavenger.

---

## 4. Crash Consistency Model

Shadow Hop guarantees **Linearizable Atomicity**. There is no intermediate state where a partial write or corrupted block is visible.

| Event | Time $T$ | Block N State | Global File State | Power Failure Result |
| :--- | :--- | :--- | :--- | :--- |
| **Start** | 0 | $k=0$ (Gen 100) | Gen 100 | File Valid (Old Version) |
| **Shadow Write** | 1 | $k=1$ (Gen 101) | Gen 100 | File Valid (Old Version). Shadow ignored. |
| **Flush Anchor** | 2 | **Anchor Updates** | **Gen 101** | **ATOMIC SWITCHOVER.** File Valid (New Version). |
| **Eclipse** | 3 | $k=0$ Freed | Gen 101 | File Valid (New Version). Space reclaimed. |

### The "Zero-Trust" Reader
Because "Old Data" exists momentarily before Eclipse, the Read Path (`hn4_read.c`) implements a strict validation gauntlet. A block is accepted **only** if:
1.  **Identity:** `Header.well_id` matches `Anchor.seed_id`.
2.  **Integrity:** `CRC32` matches.
3.  **Freshness:** `Header.generation` == `Anchor.write_gen`.

**Logic:** Reads reject any block where `Gen < Anchor.Gen`. Writes only increment `Gen`. This prevents stale reads (Ghosts) without requiring locks.

---

## 5. State Transition Visualization

### SCENARIO: Overwriting Block N

#### T0: Resting State
File is at Gen 100. Block N is at Primary Orbit ($k=0$).

```text
      [ ANCHOR (D0) ]
      Gen: 100
           |
           v (Calculated via Math)
------------------------------------------------------------------
PHYSICAL DISK:
[ ... ] [ DATA v1 (Gen 100) ] [ ... ] [ FREE SPACE ] [ ... ]
           ^
           LBA_0 (k=0)               LBA_1 (k=1)
```

#### T1: The Shadow Hop (Write)
Data written to $k=1$. Generation tagged as 101. Anchor is NOT updated yet.

```text
      [ ANCHOR (D0) ]
      Gen: 100  <-- System of Record
           |
           v
------------------------------------------------------------------
PHYSICAL DISK:
[ ... ] [ DATA v1 (Gen 100) ] [ ... ] [ DATA v2 (Gen 101) ] [ ... ]
           ^                             ^
           LBA_0                         LBA_1
           (Current Valid)               (Future / Invisible)
```

#### T2: The Anchor Update (Commit)
Anchor `write_gen` increments. Logic shifts instantly.

```text
      [ ANCHOR (D0) ]
      Gen: 101  <-- UPDATED
           |
           v
------------------------------------------------------------------
PHYSICAL DISK:
[ ... ] [ DATA v1 (Gen 100) ] [ ... ] [ DATA v2 (Gen 101) ] [ ... ]
           ^                             ^
           LBA_0                         LBA_1
           (STALE - Rejected)            (VALID - Accepted)
```

#### T3: The Eclipse (Cleanup)
Old slot is zeroed/trimmed.

```text
PHYSICAL DISK:
[ ... ] [ 00000000000000000 ] [ ... ] [ DATA v2 (Gen 101) ] [ ... ]
           ^                             ^
           LBA_0                         LBA_1
           (Sparse Hole)                 (The Truth)
```

---

## 6. Architecture Comparison: CoW vs. Hop

This table highlights the decisive architectural difference: Metadata Amplification.

| Feature | Copy-on-Write (Tree) | Shadow Hop (Vector) |
| :--- | :--- | :--- |
| **Addressing** | Pointers (Block IDs) | Equation ($G + N \cdot V$) |
| **Modification** | 1. Write Data<br>2. Update Leaf Pointer<br>3. Update Parent Pointer<br>4. Update Root | 1. Write Data ($k+1$)<br>2. Zero Old Data ($k$) |
| **Metadata I/O** | **High.** A 4KB write can cause 16KB+ of metadata updates (Write Amplification). | **Near Zero.** Only the Anchor (Root) updates. No intermediate nodes exist. |
| **Fragmentation** | **High.** Logical adjacency is lost immediately upon write. | **Controlled.** The new block ($k=1$) is mathematically related to the old one via $V$. |
| **Integrity Scrub** | **Slow ($O(Tree)$).** Must traverse pointers. | **Fast ($O(N)$).** Linear calculation. Scrubber predicts location and verifies. |
| **Garbage Collection**| Complex. Ref-counting required. | Instant. "Eclipse" frees space immediately. |

---

## 7. Orbital Recurrence (The "Toggle" Effect)

The system does not consume infinite $k$ slots. It recycles them.

**The Loop:**
1.  Write v1 at $k=0$.
2.  Write v2 at $k=1$. (Eclipse $k=0$).
3.  **Write v3:**
    *   Driver calculates $k=0$. Is it free? **YES** (Freed in Step 2).
    *   Driver writes v3 at $k=0$.
    *   Driver Eclipses $k=1$.

**Engineering Consequence:** A file undergoing heavy random I/O "toggles" between two mathematical coordinates ($LBA_X$ and $LBA_Y$). It does not spray data across the drive unless those primary slots are physically blocked by other files. This behavior is FTL-friendly, as it presents a stable logical-to-physical mapping pattern to the SSD firmware.

---

## 8. Summary of Characteristics

1.  **Latency:** Eliminates the read-modify-write cycle of B-Tree metadata nodes.
2.  **Safety:** Data is never overwritten in place. Generation counters prevent stale reads.
3.  **Determinism:** Forensic recovery can reconstruct the file state purely from the Anchor, without traversing a broken tree.
4.  **Metadata Efficiency:** The metadata overhead for a write is constant ($O(1)$), regardless of file depth or size.