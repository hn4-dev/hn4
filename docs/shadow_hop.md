# THE SHADOW HOP PROTOCOL
### *Beyond Copy-on-Write: The Physics of Mutable Ballistics*

## 1. The Core Concept

Traditional file systems rely on **Pointers**. To find a block of data, they traverse a tree: `Inode -> Indirect Block -> Physical Block`.
When you modify data in a **Copy-on-Write (CoW)** system (like ZFS or Btrfs), you cannot overwrite the live block. You write to a new location, update the pointer, update the parent pointer, and update the root. This is the **"Tree Tax."**

**HN4 rejects Pointers.** It relies on **Ballistic Equations**.
To find a block, the CPU calculates a trajectory.
The **Shadow Hop** is the mechanism by which data moves to a new physical location *without* requiring the metadata tree to be rewritten. It exploits the multi-variate nature of the Ballistic Equation to create "Orbital Shells" for data versions.

**Positioning Statement:**
*Shadow Hop is Copy-on-Write without metadata amplification — atomic mutation through mathematical relocation.*

---

## 2. The Mathematical Foundation

To understand the Hop, one must understand the Addressing Equation (Spec Section 6.1):

$$ LBA_{phys} = G + (N \times V) + \Theta(k) $$

*   **$G$ (Gravity Center):** The immutable starting point of the file.
*   **$N$ (Sequence):** The logical block number (0, 1, 2...).
*   **$V$ (Vector):** The stride/spacing between blocks.
*   **$k$ (Orbital Shell):** The Collision/Version offset. **This is the variable used by Shadow Hop.**

In a standard file, data settles at **$k=0$** (The Ground State).
When we need to modify data atomically, we do not overwrite $k=0$. We solve for **$k=1$** (The Shadow State).

### The Bounded Search Window
Critics might ask: *"Does the system search $k$ to infinity?"*
**No.**
*   The reader probes $k$ within a strict, bounded window (default: $k \in \{0..12\}$).
*   In 99.9% of workloads, files oscillate purely between $k=0$ and $k=1$.
*   **Pathological Collisions:** If all 12 orbital slots are physically blocked (a statistical anomaly), the Void Engine triggers **Vector Reseeding**. It selects a new Prime Vector $V'$ and migrates the block to a completely new trajectory, bypassing the congestion entirely.
*   **Result:** Lookup complexity is effectively $O(1)$, capped at $O(k_{max})$. It is deterministic.

---

## 3. The Shadow Hop Mechanism (Step-by-Step)

Let us visualize a write operation to **Logical Block 5** of a file.

### PHASE 1: The Trajectory Calculation
The Driver calculates the physical location of Block 5 in the **Ground State ($k=0$)**.
*   It reads the header at that location.
*   It verifies the **Generation Counter** matches the Anchor's `write_gen`.

### PHASE 2: The Hop (Allocation)
The Driver calculates the physical location for **$k=1$**.
*   If $k=1$ is occupied by another file, it tries $k=2$, then $k=3$.
*   It finds a free slot at **$k=1$**. This is the **Shadow Slot**.

### PHASE 3: The Write (Commit)
The Driver writes the **New Data** to the Shadow Slot ($k=1$).
*   **Crucial:** The Header of this new block is tagged with `Gen = Anchor.write_gen + 1`.
*   **Safety:** The old data at $k=0$ (`Gen=100`) is still valid on disk. If power fails now, the file is unchanged (Atomic).

### PHASE 4: The Anchor Update (Atomic Switch)
*   The Driver updates the **Anchor** (Metadata) in RAM.
*   It increments `Anchor.write_gen` to **101**.
*   It flushes the Anchor to disk.
*   **The Switch:** From this moment on, any reader looking for Block 5 will reject the old block (Gen 100) because it is stale, and accept the new block (Gen 101).

### PHASE 5: The Eclipse (Selective Deallocate)
Once the Anchor is secure:
*   The Driver issues an **Eclipse** to the old location at $k=0$.
*   **Method:** `NVMe Write Zeroes` (or TRIM).
*   **Result:** The old block is physically erased, freeing space immediately.
*   **Failure Mode:** If the Eclipse fails (power loss), the system remains consistent. The old block simply becomes "stale garbage" (ignored by readers) until the Scavenger cleans it up.

---

### 4. Crash Consistency & Atomicity

Shadow Hop guarantees **Linearizable Atomicity**. There is no state where a partial write is visible.

| Event | Time $T$ | State of Block $N$ | State of File | If Power Fails Here... |
| :--- | :--- | :--- | :--- | :--- |
| **Start** | 0 | At $k=0$ (Gen 100) | Gen 100 | File is valid (Old Version). |
| **Write Shadow** | 1 | Copy at $k=1$ (Gen 101) | Gen 100 | File is valid (Old Version). Shadow is ignored. |
| **Flush Anchor** | 2 | Copy at $k=1$ (Gen 101) | **Gen 101** | **ATOMIC SWITCHOVER.** File is valid (New Version). |
| **Eclipse** | 3 | $k=0$ is ERASED. | Gen 101 | File is valid (New Version). Space reclaimed. |

### The Hardware Barrier (Strict Ordering)
To guarantee this timeline, the Driver does not rely on "hope."
1.  **Shadow Write:** Data is sent to the SSD.
2.  **The Wall:** The Driver issues a **Force Unit Access (FUA)** or `NVMe Flush` command.
3.  **The Promise:** The Anchor Update (Step 4) is **never** issued until the hardware acknowledges that the Shadow Write is durable on NAND.

### The Zero-Trust Read (The Gauntlet)
Because "Old Data" exists momentarily before Eclipse, the Read Path implements a strict filter. A block is accepted **only** if it passes The Gauntlet:
*   **Magic Check:** Header must be `0x424C4B30`.
*   **Identity Check:** `Header.well_id` must match `Anchor.seed_id` (Prevents reading another file's collision).
*   **Time Check:** `Header.generation` must match `Anchor.write_gen` (Prevents reading stale Shadow Hops).
*   **Integrity Check:** `CRC32` of the payload must match.

**The Logic:** Reads **NEVER** use blocks whose generation is older than the Anchor's `write_gen`. Writes **ONLY** increase generation monotonically. This prevents ghost reads and stale cache drift.

---

## 5. Visual Illustration

### SCENARIO: Overwriting Block N

#### T0: Resting State
The file exists. Block N is at the primary trajectory ($k=0$).

```text
      [ ANCHOR (D0) ]
      Gen: 100
           |
           v (Calculated, not pointed)
------------------------------------------------------------------
PHYSICAL DISK:
[ ... ] [ DATA v1 (Gen 100) ] [ ... ] [ FREE SPACE ] [ ... ]
           ^
           LBA_0 (k=0)               LBA_1 (k=1)
```

#### T1: The Shadow Hop (Write)
We write new data. We calculate the trajectory for $k=1$. We write there.

```text
      [ ANCHOR (D0) ]
      Gen: 100  <-- Still points to old gen technically
           |
           v
------------------------------------------------------------------
PHYSICAL DISK:
[ ... ] [ DATA v1 (Gen 100) ] [ ... ] [ DATA v2 (Gen 101) ] [ ... ]
           ^                             ^
           LBA_0                         LBA_1
           (Old Valid)                   (New, Not yet committed)
```

#### T2: The Anchor Update (Commit)
We update the Anchor generation. The logic shifts instantly.

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
           (STALE - Ignored)             (VALID - Accepted)
```

#### T3: The Eclipse (Cleanup)
We wipe the old slot.

```text
PHYSICAL DISK:
[ ... ] [ 00000000000000000 ] [ ... ] [ DATA v2 (Gen 101) ] [ ... ]
           ^                             ^
           LBA_0                         LBA_1
           (Sparse Hole)                 (The Truth)
```

---

## 6. Shadow Hop vs. Copy-on-Write (The Comparison)

This is the decisive architectural difference between HN4 and ZFS/Btrfs.

| Feature | Copy-on-Write (Tree) | Shadow Hop (Math) |
| :--- | :--- | :--- |
| **Addressing** | Pointers (Block IDs) | Equation ($G + N \cdot V$) |
| **Modification** | 1. Write New Data<br>2. Update Leaf Node<br>3. Update Parent Node<br>4. Update Root Node | 1. Write New Data ($k+1$)<br>2. Zero Old Data ($k$) |
| **Metadata I/O** | **High (Write Amplification).** A 4KB write can cause 16KB+ of metadata updates. | **Near Zero.** The Anchor (Root) only updates its Timestamp/Gen. No intermediate nodes exist. |
| **Fragmentation** | **Severe.** Logical adjacency is lost immediately upon write. | **Controlled.** The new block ($k=1$) is mathematically related to the old one. |
| **Integrity Scrub** | **Slow ($O(Tree)$).** Must traverse metadata pointers to find blocks. | **Fast ($O(N)$).** Linear scan. The Scrubber calculates where blocks *should* be and verifies them against the math. |
| **Seek Overhead** | $O(\log N)$ (Tree Traversal). | $O(1)$ (Calculation). |
| **Garbage Collection**| Complex. Must ref-count blocks to know when to free. | Instant. The "Eclipse" command frees the old space immediately. |

### The FTL Advantage
Modern SSDs hate CoW because it destroys logical locality.
HN4 is the first filesystem designed **with** the FTL, not against it.
*   **Mapping Stability:** The logical relationship between Block N and Block N+1 is preserved ($V$ is constant).
*   **TRIM Hints:** The "Eclipse" command feeds directly into the SSD's internal garbage collector, reducing write amplification inside the NAND.

---

## 7. Block Reuse & The "Gravity Well"

You might ask: *"If we keep hopping to $k=1, 2, 3...$, won't we run out of orbits? Does the file explode?"*

No. The system is elastic.

### The Re-Entry (Looping)
The Shadow Hop isn't an infinite line; it's a priority queue.
1.  Write v1 at $k=0$.
2.  Write v2 at $k=1$. (Eclipse $k=0$).
3.  **Write v3:**
    *   Driver calculates $k=0$. Is it free? **YES** (We eclipsed it in step 2).
    *   Driver writes v3 at $k=0$.
    *   Driver Eclipses $k=1$.

**The Result:** A file undergoing heavy random I/O simply "toggles" or "orbits" between a few mathematical slots ($LBA_X$ and $LBA_Y$) indefinitely. It does not spray data across the drive unless those primary slots are physically blocked by other files.

### Proven Stability (Empirical Data)
Recent tests (`hn4_TEST(Shadow, Slot_Recycling_Immediate)` and `hn4_TEST(Wear, FTL_PingPong_Detection)`) confirm this behavior:
*   **Recycling:** Slots freed by a Shadow Hop are immediately available for reuse by new files or the same file.
*   **Ping-Pong Mitigation:** The Allocator logic ensures that while toggle behavior is possible for single-file benchmarks, concurrent workloads naturally spread via collision avoidance, achieving statistical wear leveling without complex algorithms.

---

## 8. Summary of Benefits

1.  **Lowest Latency:** Removes the overhead of walking/rewriting B-Trees during writes.
2.  **Atomic Safety:** Data is never overwritten in place. Crashes during write leave the old `Gen` intact.
3.  **Determinism:** At any time, forensic validation can recompute every block location from the Anchor alone. **There is no hidden state.**
4.  **No GC Dependency:** The filesystem does not rely on background Garbage Collection for correctness, only for efficiency. Correctness is guaranteed by the Generation Counter.