# NON-CONTIGUOUS ALLOCATION IN HN4 ARCHITECTURE
### *Analysis of Deterministic Placement on Solid State Storage*

**DOCUMENT ID:** HN4-THEORY-001
**STATUS:** ARCHITECTURAL ANALYSIS
**TARGET:** SYSTEMS ARCHITECTS / PERFORMANCE ENGINEERS

---

## 1. The Fragmentation Variance

In traditional file system design, fragmentation (non-contiguous data placement) is considered a performance defect. It implies disorder and increased latency. Tools are designed specifically to consolidate data blocks.

**HN4 proposes a different approach:**
On modern NVMe/NAND storage, **Sequentiality is often a bottleneck.**
**Uniform Distribution (Controlled Scattering) allows for channel saturation.**

In the HN4 architecture, non-contiguous placement is not a defect to be solved; it is a **design choice** utilized to maximize parallel throughput on solid-state media.

---

## 2. Legacy Constraints: Why Fragmentation Was Costly

To understand the HN4 approach, we must review the constraints of legacy storage that HN4 replaces.

### A. The Mechanical Penalty (HDD)
On a rotational hard drive, reading Logical Block Address (LBA) 100 followed by LBA 1,000,000 requires moving a physical read head.
*   **Seek Time:** ~8ms to 12ms.
*   **Impact:** Heavily fragmented files degrade throughput significantly due to mechanical latency.

### B. The Metadata Overhead (B-Trees and Extents)
Filesystems like **NTFS**, **Ext4**, and **XFS** track file locations using **Extents** (Start LBA, Length).
*   **Contiguous File:** 1 Extent (Fast metadata lookup).
*   **Fragmented File:** Thousands of Extents.
*   **The Cost:** Metadata size grows linearly with fragmentation. To read a fragmented file, the OS must traverse a B-Tree structure.
*   **Result:** The CPU spends cycles querying *where* the data is before it can issue the I/O command.

```text
TRADITIONAL FS (Ext4/XFS) - The Lookup Problem

[ INODE ] -> [ Extent: 0-100 @ LBA 500 ]
          -> [ Extent: 101-200 @ LBA 9000 ] -> [ Indirect Block ] -> [ Extent... ]
          -> [ ... ]

Lookup Complexity: O(log E) where E is the number of fragments.
RAM Usage: Increases with fragmentation.
```

---

## 3. The HN4 Solution: Algorithmic Addressing

HN4 removes the variable metadata cost. It does not store a list of locations; it stores a **Generation Formula**.

### The Addressing Function
The physical location of any block is derived mathematically via a Linear Congruential Generator (LCG) logic (see `_calc_trajectory_lba` in `hn4_allocator.c`).

$$ LBA_{phys} = LBA_{base} + \left( [ (G / S) + (N \times V) + \Theta(k) ] \pmod \Phi \right) \times S $$

*   **$G$ (Base Offset):** The starting seed LBA for the file.
*   **$N$ (Sequence Index):** The logical block number (0, 1, 2...).
*   **$V$ (Stride Vector):** The step size between blocks.
*   **$\Theta(k)$ (Collision Offset):** A deterministic offset used if the primary location is occupied.
*   **$\Phi$ (Window):** The total capacity of the allocation region.
*   **$S$ (Scale):** The block size multiplier (e.g., 4KB, 64KB).

Regardless of whether a file is contiguous or scattered across the drive, the **Metadata Size remains constant (128 Bytes).**

### Deterministic Latency

Traditional systems degrade as fragmentation increases because the metadata tree deepens. HN4 performance remains constant because calculating an address takes a fixed number of CPU instructions regardless of the physical location.

```text
LATENCY (µs)
^
|
|                                     / (B-Tree FS - Metadata Lookup Cost)
|                                   /
|                                 /
|                               /
|                             /
|                           /
|-------------------------+----------------------- (HN4 - Math Calculation)
|
+---------------------------------------------------->
  FRAGMENTATION LEVEL (Number of Non-Contiguous Blocks)
```

### Complexity Analysis ($O(1)$)
*   **Scenario:** Accessing Block 50,000 of a scattered file.
*   **HN4 Logic:**
    1.  Load Anchor (128 Bytes) from memory.
    2.  Execute Formula: `(G + (50000 * V)) % Capacity`.
    3.  Verify allocation in Bitmap (1 bit check).
*   **Result:** Zero tree traversal. Zero pointer chasing.

---

## 4. Hardware Optimization: Channel Parallelism

Modern NVMe SSDs are essentially **Internal RAID Arrays**.
*   A typical SSD has 8 to 16 internal NAND Channels.
*   Each Channel manages multiple NAND Dies.

### The Sequential Bottleneck ($V=1$)
If a file is written sequentially (LBA 1, 2, 3, 4...):
*   The workload hits logical addresses in order.
*   While SSD controllers attempt to stripe this, heavy sequential writes can bottleneck on specific dies if internal Garbage Collection is active.

### The Stride Distribution ($V \ne 1$)
HN4 selects a **Prime Stride ($V$)** relative to the capacity.
*   **Example:** $V = 17$.
*   **Pattern:** Block 0 @ 0, Block 1 @ 17, Block 2 @ 34...
*   **Physical Result:** I/O requests are mathematically distributed across the Logical Address Space.

**This is "Controlled Distribution."**
By scattering I/O mathematically, HN4 increases the probability that a multi-block read request will engage **multiple NAND Channels simultaneously**, maximizing internal parallelism.

```text
VISUALIZATION: 4-CHANNEL SSD

[ Sequential File ]      [ HN4 Distributed File ]
Channel 1: [AAAA]        Channel 1: [A...]
Channel 2: [....]        Channel 2: [.A..]
Channel 3: [....]        Channel 3: [..A.]
Channel 4: [....]        Channel 4: [...A]

Result: Single Channel   Result: Multi-Channel Saturation
```

---

## 5. Relocation on Write: Managing Updates

When a file block is modified, HN4 uses a **Relocation** strategy (similar to Copy-on-Write). It calculates a new position rather than overwriting in place.

### The Collision Iterator ($k$)
When the primary calculated slot ($k=0$) is occupied or being updated, the data is written to the next deterministic shell ($k=1$).
*   **Physical Layout:** Blocks are not adjacent.
*   **Performance Impact:** Negligible.
    *   CPU calculation for $k=1$ is effectively instantaneous.
    *   NVMe random read latency (~20µs) is comparable to sequential read latency.
    *   There is no mechanical seek penalty.

### Parallel Probing
To maintain performance during reads, the driver uses a **Parallel Probe** (Shotgun Protocol in `hn4_read.c`).
*   The driver calculates locations for $k=0, 1, 2, 3$ simultaneously.
*   It issues I/O requests for likely candidates in parallel.
*   **Latency:** Determined by the fastest successful read response.

---

## 6. Comparison Matrix

| Feature | Ext4 / NTFS | ZFS / Btrfs (CoW) | HN4 (Algorithmic) |
| :--- | :--- | :--- | :--- |
| **Addressing** | Extent Lists / B-Trees | Block Pointers | **Math Function** |
| **Metadata Scaling** | Linear (w/ fragments) | Linear (w/ fragments) | **Constant (O(1))** |
| **Read Complexity** | $O(\log N)$ | $O(\log N)$ | **$O(1)$** |
| **Write Strategy** | Overwrite (mostly) | Copy-on-Write | **Calculated Relocation** |
| **SSD Optimization** | Controller Dependent | Transaction Groups | **Native Stride Hashing** |
| **Defrag Required?** | **YES** (Performance) | **YES** (Space) | **NO** (Performance) |

---

## 7. Handling Boundary Conditions

There are specific hardware scenarios where scattering is detrimental. HN4 handles these via **Profiles**.

### Case A: Rotational Media (HDD / Tape)
*   **Constraint:** Mechanical seek times (~10ms) make non-contiguous access prohibitively slow.
*   **HN4 Adaptation:**
    *   Driver detects `HN4_HW_ROTATIONAL` flag.
    *   Forces Stride $V=1$ (Strict Sequential).
    *   Disables Parallel Probing.
    *   This forces HN4 to behave like a traditional linear filesystem.

### Case B: High Utilization (The Horizon)
*   **Constraint:** If the drive is >90% full (`_check_saturation`), mathematical collision rates rise, requiring too many CPU retries to find a free slot.
*   **HN4 Adaptation:**
    *   The Allocator switches to the **Overflow Log** (Horizon/D1.5).
    *   Allocation becomes a simple linear append ($V=0$) into a reserved ring buffer.
    *   **Recovery:** When space is freed (<90%), the **Scavenger** process recalculates optimal positions and moves data back to the primary region.

---

### Summary Note on Wear Leveling

> **HN4 complements the SSD Firmware.**
> While SSDs have internal Flash Translation Layers (FTL) for wear leveling, HN4's distributed write patterns reduce the workload on the FTL by avoiding "hot spots" in the Logical Address Space.
>
> **Physics Limitations:**
> As capacity approaches 100%, performance will degrade as the system reverts to linear searching (Horizon Mode). This is a physical inevitability of storage, handled gracefully by the fallback mechanism.

---

## 8. Conclusion: Architecture Summary

In the HN4 architecture, standard "fragmentation" concerns are mitigated by the characteristics of solid-state media:

1.  **Uniform Wear:** Distributed writes naturally utilize the entire address space.
2.  **Deterministic Latency:** Addressing cost is fixed and does not degrade over time.
3.  **Maximum Bandwidth:** Scattering data allows for better utilization of internal SSD parallelism.

**In this model, non-contiguous placement is a deliberate distribution strategy.**
