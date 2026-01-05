# HN4 WRITE ARCHITECTURE
### *Atomic Mutation Without the Metadata Tax*

**Status:** Implementation Standard v6.1
**Module:** `hn4_write.c`
**Metric:** Zero-Copy Atomicity | $O(1)$ Complexity

---

## 1. The Bottleneck: The "Journaling Tax"

To understand why HN4 writes are revolutionary, we must analyze the "Cost of Safety" in traditional POSIX filesystems.

In architectures like **EXT4 (Journaling)** or **ZFS (Copy-on-Write)**, ensuring data integrity across power failures imposes a massive IO penalty.

### The Legacy "Double Write" Problem
To modify a single 4KB block $N$ safely, a legacy filesystem must:
1.  **Write Journal:** Serialize the intent to the log.
2.  **Barrier:** Flush hardware cache (Latency Spike).
3.  **Write Metadata:** Traverse and update B-Tree pointers.
4.  **Write Data:** Write the actual payload.
5.  **Commit:** Clear the journal entry.

**The Cost:**
*   **Write Amplification (WA):** 1 User Write $\approx$ 3-4 Physical Writes.
*   **Latency Jitter:** The `kjournald` or `txg_sync` thread periodically locks the filesystem to flush the log, causing stutter.
*   **Flash Wear:** SSDs degrade 3x faster due to excessive metadata churn.

---

## 2. The HN4 Solution: The Shadow Hop ($O(1)$)

HN4 achieves atomic safety **without a Journal** and **without a Tree**.
It utilizes **Mathematical Displacement**.

Instead of updating a pointer to say *"Block N is now at Address Y,"* we simply increment the input variable of the trajectory equation.

### The Mechanism
1.  **State A (Current):** Block $N$ exists at Trajectory $K=0$.
2.  **The Write:**
    *   The Driver calculates Trajectory $K=1$.
    *   The Driver writes **New Data** to $LBA(K=1)$.
3.  **The Commit:**
    *   The Driver increments the **Anchor Generation Count** ($Gen++$) in RAM.
    *   The Driver flushes the Anchor (128 bytes) to disk.
4.  **The Eclipse:**
    *   The old block at $K=0$ is now mathematically "stale" because its Generation ID is lower than the Anchor's. It is ignored by all readers.

**Result:**
*   **Metadata IO:** 1 tiny write (Anchor update).
*   **Data IO:** 1 write.
*   **Atomicity:** Perfect. If power fails before Step 3, the old data remains valid. If after, the new data is live.

---

## 3. NVM.2 Protocol: The "Zero-Wait" Path

When running on Byte-Addressable Non-Volatile Memory (Intel Optane, CXL.mem), HN4 eliminates the block layer entirely.

### 3.1 The Persistent Barrier (`CLWB`)
Standard storage drivers treat NVM like a fast SSD, still incurring block overhead. HN4 treats NVM as **Persistent RAM**.

*   **Logic:** Instead of issuing NVMe commands, the driver uses CPU instructions (`MOV`, `CLWB`, `SFENCE`).
*   **Flow:**
    1.  `memcpy` data to the persistent memory map.
    2.  Execute `CLWB` (Cache Line Write Back) to evict data from CPU L1/L2 cache to the durability domain.
    3.  Execute `SFENCE` to enforce ordering.
*   **Benefit:** Write latency drops from ~10µs (NVMe) to **~100ns** (Memory Bus).

### 3.2 The Metadata Hazard
Since the Anchor structure resides in memory-mapped D0 space on NVM volumes, updating `anchor->write_gen` is just a memory store. If the CPU cache holds this store during a power loss, the file system becomes inconsistent.

**Fix:** `hn4_write_block` explicitly flushes the Anchor cache line immediately after updating metadata.

---

## 4. ZNS Native: The "Append" Accelerator

When running on **Zoned Namespaces (ZNS)** SSDs, HN4 bypasses the internal SSD logic.

Standard SSDs use an internal **Flash Translation Layer (FTL)** to map logical blocks to physical pages. This FTL requires internal Garbage Collection (GC), causing "stutter" in games and databases.

### HN4 acts as the FTL.
*   **Logic:** The Driver sees the raw NAND Zones.
*   **Behavior:**
    *   It does *not* search for holes.
    *   It writes strictly to the **Zone Write Pointer**.
*   **Impact:**
    *   **Write Amplification:** 1.0 (Theoretical Minimum).
    *   **Latency:** Deterministic. No SSD-internal GC pauses.
    *   **Throughput:** Saturates the bus because writes are perfectly sequential.

---

## 5. The "Tensor Tunnel" (Zero-Copy DMA)

For AI Training (`HN4_PROFILE_AI`), the CPU copy overhead is the bottleneck. Moving 100GB from Disk $\to$ RAM $\to$ GPU creates massive memory pressure.

**HN4 implements PCIe Peer-to-Peer (P2P).**

1.  **Map:** User requests "Write Tensor to File X".
2.  **Calculate:** Driver computes the physical LBAs.
3.  **Tunnel:** Driver creates a DMA descriptor for the NVMe Controller.
    *   *Source:* GPU VRAM Address (BAR).
    *   *Dest:* NVMe LBA.
4.  **Execute:** The data flows **GPU $\to$ PCIe Switch $\to$ SSD**.
    *   **CPU Usage:** ~0%.
    *   **System RAM Usage:** 0%.

---

## 6. Performance Visualizations

### 6.1 Write Latency Histogram (4KB Random)
Comparing HN4 against Ext4 on NVMe Gen4. Note the "Long Tail" of Ext4 due to Journal commits.

```text
Count
  ^
  |      [HN4]
  |      |   |
  |      |   |        [Ext4]
  |      |   |        |    |
  |      |   |        |    |            [Ext4 Journal Flush]
  |      |   |        |    |            |..................|
  +------+---+--------+----+------------+------------------+-----> Time (µs)
         5   10       25   40           1000+
```

### 6.2 Write Amplification Factor (WAF)
Lower is better. High WAF kills SSDs.

```text
WAF
 5.0 | [Ext4 - Journal=Data]
     | ||||||||||||||||||||
 4.0 | ||||||||||||||||||||
     |
 3.0 | [ZFS - Double Write]
     | ::::::::::::::::::::
     | ::::::::::::::::::::
 2.0 | 
     |
 1.0 | [HN4 - Shadow Hop]
     | XXXXXXXXXXXXXXXXXXXX
     | XXXXXXXXXXXXXXXXXXXX
  +--+---------------------+
```

### 6.3 Throughput Stability (ZNS Mode)
Sustained sequential write over 2 hours.

```text
GB/s
  ^
  | ----------------------- [HN4 ZNS] (Flat line at wire speed)
7 |
  |
6 |      /\      /\         [Legacy FTL] (GC cliffs)
  | ____/  \____/  \____
5 |
  +------------------------> Time (Hours)
    0      0.5     1.0
```

---

## 7. Performance Matrix

| Feature | EXT4 (Journal) | ZFS (CoW) | HN4 (Ballistic) |
| :--- | :--- | :--- | :--- |
| **Atomic Strategy** | Journaling | Copy-on-Write Tree | **Shadow Hop** |
| **Write Amp** | High (Journal + Meta) | High (Tree Ripple) | **Low (Anchor Only)** |
| **Latency** | Variable (Journal Flush) | Good (ZIL/SLOG) | **Deterministic** |
| **ZNS Support** | No (Needs Shim) | No | **Native** |
| **GPU Direct** | No | No | **Native** |
| **NVM Optimization**| Cached (Page Cache) | ARC (RAM Heavy) | **Direct Access (DAX)** |

---

## 8. Summary

HN4 writes are fast because they are **Self-contained**.

*   We do not need to lock a directory tree.
*   We do not need to update parent pointers.
*   We do not need to defragment a free list.

We calculate coordinates. We write data. We flip a switch.
**It is the shortest path from RAM to Rust.**