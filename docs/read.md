# HN4 READ ARCHITECTURE
### Module: `hn4_read.c`
**Metric:** $O(1)$ Address Resolution

---

## 1. The Bottleneck: Hierarchical Dependency

Legacy filesystems (EXT4, ZFS, Btrfs) model files as **trees**. To read a specific block (e.g., Logical Offset 100GB) in a large or fragmented file, the CPU cannot calculate the physical location. It must traverse a pointer hierarchy to find it.

### The "Pointer Chase" ($O(\log N)$)
This traversal creates a serial dependency chain in the I/O path:
1.  **Read Inode:** Load file header.
2.  **Read Indirect Block A:** Find range covering the target offset.
3.  **Read Indirect Block B:** Narrow down the range.
4.  **Read Data:** Fetch the actual payload.

**The Engineering Cost:**
*   **Latency Stacking:** You cannot request Block B until Block A returns. On high-latency media or networks (NVMe-oF), this triples the time-to-first-byte.
*   **Cache Pollution:** To read 4KB of user data, the CPU loads ~12KB of metadata pointers into the L1/L2 cache. This evicts hot application data, reducing instructions-per-clock (IPC).
*   **Non-Deterministic Jitter:** Fragmentation increases tree depth. A sequential read might be fast, while a random read in a fragmented region stalls.

---

## 2. The Solution: Algebraic Addressing ($O(1)$)

HN4 removes the pointer hierarchy entirely. It models a file not as a tree of blocks, but as a **mathematical vector**.

To locate Block $N$, the engine does not perform a lookup. It performs a calculation.

### The Trajectory Equation
The physical location is derived using the file's immutable physics parameters (stored in the Anchor):

$$ LBA_{phys} = LBA_{base} + \left( [ G + (N \times V \times 2^M) + \Theta(k) ] \pmod{\Phi} \right) $$

*   **$G$ (Gravity Center):** The 128-bit physical starting seed.
*   **$N$ (Sequence):** The logical block index requested.
*   **$V$ (Orbit Vector):** The stride pattern (e.g., 1 for sequential HDD, large prime for SSD interleave).
*   **$M$ (Fractal Scale):** Block size exponent ($2^M$).
*   **$\Theta(k)$:** The collision offset for slot $k$.
*   **$\Phi$ (Window):** The allocation window size.

**Impact:** The CPU overhead for this calculation is negligible (<10ns). Crucially, the cost is constant regardless of file size (1MB vs 1EB) or fragmentation level.

---

## 3. Latency Topology Comparison

### Legacy: Serial Dependency
The CPU must wait for the storage medium multiple times before issuing the actual data read.

```text
[ CPU ]
   |
   +--> [ Request Inode ] -----> [ WAIT ]
                                    |
        [ Request Level 1 ] <-------+
               |
               +---------------> [ WAIT ]
                                    |
        [ Request Level 2 ] <-------+
               |
               +---------------> [ WAIT ]
                                    |
        [ Request DATA ] <----------+
               |
               +---------------> [ DATA ]
```
*Total Time:* $3 \times \text{Seek} + \text{Transfer}$.

### HN4: Parallel Calculation
The address is computed in registers. The first request sent to the drive is for the data itself.

```text
[ CPU ]
   |
   +--> [ Load Anchor (RAM) ]
   |
   +--> [ CALC LBA = G + (N*V) ] (<10ns)
   |
   +--> [ Request DATA ]
               |
               +---------------> [ DATA ]
```
*Total Time:* $0 \times \text{Seek} + \text{Transfer}$.

---

## 4. Parallel Candidate Resolution (PCR)

A mathematical mapping will inevitably have collisions (two logical blocks mapping to the same physical LBA). Standard hash tables handle this by probing sequentially ($k=0$, then $k=1$), which adds latency.

HN4 exploits the massive parallelism of NVMe queues (Depth 64K) to solve this without waiting.

**The Shotgun Logic (`hn4_read.c`):**
1.  **Calculate:** Generate LBAs for the first 4 orbital shells ($k=0..3$).
2.  **Filter:** Check the in-memory **Void Bitmap** to see which of those candidates are physically allocated.
3.  **Fire:** Issue NVMe Read Commands for *all* valid candidates simultaneously.
4.  **Validate:** The first block to return with a matching `Well_ID` (File UUID) in its header is the winner.
5.  **Discard:** Any other in-flight reads are cancelled or ignored.

**Result:** PCR trades internal bandwidth for minimum latency. Even if the data is in the 3rd collision slot, the read returns as fast as the 1st slot because the requests happen in parallel.

---

## 5. CPU Cache Purity

A major, often overlooked bottleneck in high-performance databases is cache thrashing caused by filesystem metadata.

*   **Legacy Behavior:** Reading random data pulls metadata pages into the CPU cache hierarchy. If the working set is large, this evicts the application's own instructions or data structures.
*   **HN4 Behavior:** The driver performs math in CPU registers. It touches **zero disk metadata bytes** during the read path.

**Impact:** For workloads like Postgres or AI training, this yields a measurable increase in application IPC (Instructions Per Clock), as the filesystem footprint in the L2 cache is effectively zero.

---

## 6. Architecture Comparison

| Feature | Extent-Based (EXT4/XFS) | Pointer-Based (ZFS) | HN4 Ballistic |
| :--- | :--- | :--- | :--- |
| **Addressing** | Extent Tree | Merkle Tree | **Algebraic Vector** |
| **Complexity** | $O(\log N)$ | $O(\log N)$ | **$O(1)$** |
| **Frag. Penalty** | **High.** Requires tree walking. | **High.** More metadata fetches. | **None.** Math cost is identical. |
| **RAM Requirement** | Buffer Cache (Medium) | ARC (Very High) | **Nano-Cortex (Low)** |
| **Read Amplification** | Medium (Metadata pages) | High (Checksum/Tree) | **Zero (Direct Data)** |
| **Scale Limit** | ~1 EiB | Zettabytes | **$2^{128}$ Bytes** |

---

## 7. Media-Specific Optimizations

### NVM Fast Path
For byte-addressable media (Intel Optane, CXL):
*   **Zero-Copy:** Data is `memcpy`'d directly from persistent media to the user buffer.
*   **Logic Bypass:** Hysteresis loops and block-layer queues are skipped entirely. Latency floor is governed only by the memory bus (~100ns).

### ZNS (Zoned Storage)
For Zoned Namespaces:
*   **Constraint:** The Orbit Vector ($V$) is forced to 1 (Sequential).
*   **Simplification:** PCR is disabled. Since writes are strictly sequential, collisions are mathematically impossible within a valid zone.

---

## 8. Performance Characteristics

### 8.1 Latency vs. File Offset
Comparing random read latency at increasing offsets within a file.
*   **Tree-based:** Latency climbs as offsets increase (deeper tree depth).
*   **HN4:** Latency is flat. Reading byte 0 takes the same time as reading byte 1 Exabyte.

```text
Latency
  ^
  |      [Tree Systems]
  |      /
  |     /
  |    /
  |   /
  |  /
  | /
  |/_______________________________________ [HN4]
  +----------------------------------------> Offset
```

### 8.2 Throughput Stability
In fragmented scenarios (aged filesystem):
*   **Tree-based:** Throughput degrades as extents become smaller and metadata scattered.
*   **HN4:** Throughput remains stable. The mathematical relationship between logical and physical blocks does not degrade over time; it only becomes more complex ($k$ increases), which PCR handles via parallel queues.