# HN4 WRITE ARCHITECTURE
### *Atomic Mutation Without the Metadata Penalty*

**Status:** Implementation Standard v6.1
**Module:** `hn4_write.c`
**Metric:** Zero-Copy Atomicity | $O(1)$ Complexity

---

## 1. The Bottleneck: The Journaling Overhead

To understand the efficiency of HN4 write operations, we must analyze the performance costs inherent in traditional POSIX filesystems.

In architectures like **EXT4 (Journaling)** or **ZFS (Copy-on-Write)**, ensuring data integrity across power failures imposes a significant I/O penalty.

### The Legacy "Double Write" Problem
To modify a single 4KB block $N$ safely, a legacy filesystem typically performs the following sequence:
1.  **Journal Write:** Serialize the intent to the transaction log.
2.  **Barrier:** Flush hardware cache (Latency Spike).
3.  **Metadata Update:** Traverse and update B-Tree pointers.
4.  **Data Write:** Write the actual payload to the new location.
5.  **Commit:** Clear the journal entry.

**The Cost:**
*   **Write Amplification (WA):** 1 User Write results in $\approx$ 3-4 Physical Writes.
*   **Latency Jitter:** Background threads (e.g., `kjournald`, `txg_sync`) periodically lock the filesystem to flush the log, causing IO stalls.
*   **Flash Wear:** SSD endurance is consumed at an accelerated rate due to excessive metadata churn.

---

## 2. The HN4 Solution: Atomic Relocation ($O(1)$)

HN4 achieves atomic safety **without a Journal** and **without a B-Tree**.
It utilizes **Mathematical Addressing**.

Instead of updating a pointer structure to reflect a new block address, the system simply increments the input variable of the addressing equation.

### The Mechanism
1.  **State A (Current):** Block $N$ exists at Address $K=0$.
2.  **The Write:**
    *   The Driver calculates the next address $K=1$ using the addressing function.
    *   The Driver writes **New Data** to $LBA(K=1)$.
3.  **The Commit:**
    *   The Driver increments the **Anchor Generation Count** ($Gen++$) in memory.
    *   The Driver flushes the Anchor (128 bytes) to disk.
4.  **The Eclipse:**
    *   The old block at $K=0$ is now logically "stale" because its Generation ID is lower than the Anchor's current generation. It is effectively orphaned and ignored by all readers.

**Result:**
*   **Metadata IO:** 1 minimal write (Anchor update).
*   **Data IO:** 1 payload write.
*   **Atomicity:** Guaranteed. If power fails before Step 3, the old data remains valid. If after, the new data is authoritative.

---

## 3. NVM Protocol: Direct Access Path

When running on Byte-Addressable Non-Volatile Memory (e.g., Intel Optane, CXL.mem), HN4 bypasses the block layer entirely.

### 3.1 The Persistent Barrier (`CLWB`)
Standard storage drivers treat NVM as a fast SSD, incurring block overhead. HN4 treats NVM as **Persistent RAM**.

*   **Logic:** Instead of issuing NVMe commands, the driver uses CPU store instructions and cache control.
*   **Flow:**
    1.  `memcpy` data to the persistent memory map.
    2.  Execute `CLWB` (Cache Line Write Back) to evict data from CPU L1/L2 cache to the durability domain (ADR).
    3.  Execute `SFENCE` to enforce ordering.
*   **Benefit:** Write latency drops from ~10µs (NVMe) to **~100ns** (Memory Bus).

### 3.2 The Metadata Hazard
Since the Anchor structure resides in memory-mapped space on NVM volumes, updating `anchor->write_gen` is a standard memory store. If the CPU cache holds this store during a power loss, the file system becomes inconsistent.

**Fix:** `hn4_write_block` explicitly flushes the Anchor cache line (`CLWB`) immediately after updating the generation counter.

---

## 4. ZNS Native: Sequential Append Optimization

When running on **Zoned Namespaces (ZNS)** SSDs, HN4 bypasses the internal SSD Flash Translation Layer (FTL).

Standard SSDs map logical blocks to physical pages internally, requiring Garbage Collection (GC) which causes unpredictable latency.

### HN4 Manages Physical Zones
*   **Logic:** The Driver addresses raw NAND Zones directly.
*   **Behavior:**
    *   It does *not* search for free blocks.
    *   It writes strictly to the **Zone Write Pointer** via Append commands.
*   **Impact:**
    *   **Write Amplification:** 1.0 (Theoretical Minimum).
    *   **Latency:** Deterministic. No SSD-internal GC pauses.
    *   **Throughput:** Maximizes bus saturation due to strictly sequential write patterns.

---

## 5. Tensor Tunneling (Zero-Copy DMA)

For AI Training workloads (`HN4_PROFILE_AI`), CPU copy overhead is a bottleneck. Moving large datasets from Disk $\to$ RAM $\to$ GPU creates memory pressure.

**HN4 implements PCIe Peer-to-Peer (P2P) DMA.**

1.  **Map:** User requests transfer of file data to GPU buffer.
2.  **Calculate:** Driver computes the physical LBAs.
3.  **Tunnel:** Driver creates a DMA descriptor for the NVMe Controller.
    *   *Source:* NVMe LBA.
    *   *Dest:* GPU VRAM Address (BAR).
4.  **Execute:** The data flows **SSD $\to$ PCIe Switch $\to$ GPU**.
    *   **CPU Usage:** ~0%.
    *   **System RAM Usage:** 0%.

---

## 6. Performance Characterization

### 6.1 Write Latency Histogram (4KB Random)
Comparison against Ext4 on NVMe Gen4. Note the "Long Tail" of Ext4 due to Journal commits.

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
Lower is better for SSD endurance.

```text
WAF
 5.0 | [Ext4 - Journal + Metadata]
     | ||||||||||||||||||||
 4.0 | ||||||||||||||||||||
     |
 3.0 | [ZFS - CoW Tree Updates]
     | ::::::::::::::::::::
     | ::::::::::::::::::::
 2.0 | 
     |
 1.0 | [HN4 - Atomic Relocation]
     | XXXXXXXXXXXXXXXXXXXX
     | XXXXXXXXXXXXXXXXXXXX
  +--+---------------------+
```

### 6.3 Throughput Stability (ZNS Mode)
Sustained sequential write performance over 2 hours.

```text
GB/s
  ^
  | ----------------------- [HN4 ZNS] (Consistent wire speed)
7 |
  |
6 |      /\      /\         [Legacy FTL] (GC induced throttling)
  | ____/  \____/  \____
5 |
  +------------------------> Time (Hours)
    0      0.5     1.0
```

---

## 7. Feature Matrix

| Feature | EXT4 (Journal) | ZFS (CoW) | HN4 (Algorithmic) |
| :--- | :--- | :--- | :--- |
| **Atomic Strategy** | Journaling | Copy-on-Write Tree | **Atomic Relocation** |
| **Write Amp** | High (Journal + Meta) | High (Tree Ripple) | **Low (Anchor Only)** |
| **Latency** | Variable (Journal Flush) | Good (ZIL/SLOG) | **Deterministic** |
| **ZNS Support** | No (Needs Shim) | No | **Native** |
| **GPU Direct** | No | No | **Native** |
| **NVM Optimization**| Cached (Page Cache) | ARC (RAM Heavy) | **Direct Access (DAX)** |

---

## 8. Summary

HN4 writes achieve high performance through **Self-Containment**.

*   No directory tree locking.
*   No parent pointer updates.
*   No free list defragmentation during write path.

The system calculates coordinates, writes data, and atomically updates the version counter. This represents the shortest possible path from RAM to persistent storage.
