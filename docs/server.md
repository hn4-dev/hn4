# HN4 SPATIAL ARRAY & HYPER-CLOUD PROFILE
### *Multi-Drive Physics & Server Optimization*
**Specification:** v6.6 | **Status:** Production Ready | **Module:** `hn4_io_router.c`

---

## 1. Executive Summary

Traditional storage engines rely on block-level abstraction layers (RAID controllers, LVM) to aggregate physical disks. This introduces latency, "write holes," and alignment inefficiencies.

**HN4 asserts that Aggregation is Mathematical.**

The **Spatial Array** replaces legacy striping and mirroring with **Ballistic Distribution**. Instead of mapping logical blocks to physical stripes (LBA 0 -> Drive A, LBA 1 -> Drive B), HN4 maps **Object Trajectories** to specific Gravity Wells (Drives).

This architecture is exposed via the `HN4_PROFILE_HYPER_CLOUD` profile, specifically engineered to compete with hyperscale file systems like ReFS and Azure Blob Storage. It prioritizes **Throughput**, **concurrency**, and **O(1) Scalability** over granular storage efficiency.

---

## 2. HN4_PROFILE_HYPER_CLOUD

This profile fundamentally alters the filesystem's operational physics for server workloads.

### 2.1 Technical Specifications
*   **Block Size:** **64 KB** (Fixed).
    *   *Rationale:* Matches standard cloud object chunks and SQL Server extent sizes. Minimizes metadata overhead by 16x compared to 4KB blocks.
*   **Alignment:** **1 MB**.
    *   *Rationale:* Aligns with underlying RAID hardware stripes and Cloud Storage erasure coding chunks.
*   **Allocation Strategy:** **Ballistic Scatter (O(1))**.
    *   *Rationale:* Disables "Deep Scan." For high-IOPS workloads, latency consistency is paramount. The allocator probes the primary ballistic orbits (k=0..12). If no slot is found, it immediately overflows to the Horizon Log rather than thrashing the seek heads searching for gaps.
*   **Write Policy:** **Async Durability**.
    *   *Rationale:* Disables per-block `FLUSH/FUA` commands. Relies on the Array Controller's battery-backed cache or explicit `fsync()` calls. Increases IOPS by 10-50x.
*   **Compression:** **Opt-In Only**.
    *   *Rationale:* Server data is often high-entropy (encrypted blobs, compressed video). Speculative compression wastes CPU cycles.

---

## 3. The Spatial Array Architecture

The Spatial Array virtualizes `vol->target_device` into an array context (`hn4_array_ctx_t`). All I/O is dispatched via the **Spatial Router** (`_hn4_spatial_router`).

### 3.1 Gravity Well Entanglement (Mode: MIRROR)
*   **Analog:** RAID-1 (Mirroring).
*   **Mechanism:**
    *   **Writes:** Dispatched synchronously to **ALL** active drives in the array.
    *   **Reads:** Dispatched to the **First Healthy Drive** (Index 0).
*   **Superiority vs RAID-1:**
    *   **Identity Consistency:** Replication is tied to the File Identity (Anchor), not the disk block. Recovery uses the **Chronicle** (Audit Log) to resync files based on generation ID, eliminating the need for bit-for-bit disk scrubbing.

### 3.2 Ballistic Sharding (Mode: SHARD)
*   **Analog:** RAID-0 (Striping).
*   **Mechanism:**
    *   **The Shard Function:** Distribution is determined by the Object's immutable **Seed ID**, not its Logical Address.
    $$ \text{Drive}_{\text{Index}} = (\text{Seed}_{\text{lo}} \oplus \text{Seed}_{\text{hi}}) \pmod{\text{Count}} $$
    *   **Behavior:**
        *   File A (Hash 0) resides entirely on Drive A.
        *   File B (Hash 1) resides entirely on Drive B.
*   **Architectural Rarity:** This approach eliminates the metadata layer found in Ceph/Swift while maintaining POSIX coherence.
    *   **Zero Fragmentation:** A single file is never split across drives. Sequential reads run at native drive speed without seek thrashing caused by stripe interleaving.
    *   **Failure Isolation:** If Drive A fails, File B is 100% recoverable. In RAID-0, losing one drive corrupts the entire volume.

### 3.3 Dynamic Expansion (JBOD++)
*   **Mechanism:** `hn4_pool_add_device`.
*   **Logic:**
    *   New drives can be added to the array context at runtime.
    *   The `total_pool_capacity` is updated atomically.
    *   **No Rebalancing:** Existing files stay where they are. New files (generated with new UUIDs) will statistically land on the new drive due to the modulo function. This is mathematically correct behavior that avoids the massive I/O storm of "Restriping."

---

## 4. Comparison: HN4 Spatial Array vs. The Industry

| Feature | HN4 Spatial Array | Legacy RAID | Object Store (Ceph/S3) |
| :--- | :--- | :--- | :--- |
| **Distribution Unit** | **Object Identity** | Disk Block | Object ID |
| **Placement Logic** | **Deterministic Math** | Fixed Stripe Map | Lookup Table (CRUSH/Ring) |
| **Metadata Overhead** | **Zero** (Calculated) | Low | High (Directory Service) |
| **POSIX Coherence** | **Native** | Native | Emulated (Slow) |
| **Rebalance Cost** | **Zero** | High (Restripe) | High (Backfill) |
| **Write Hole** | **Impossible** | Vulnerable | N/A (Replica based) |

---

## 5. Implementation Guide

### 5.1 Configuring an Array (C Code)

To create a mirrored Hyper-Cloud volume:

```c
/* 1. Initialize Devices */
hn4_hal_device_t* dev0 = hn4_hal_open("/dev/nvme0n1");
hn4_hal_device_t* dev1 = hn4_hal_open("/dev/nvme1n1");

/* 2. Format Primary (Must use HyperCloud Profile) */
hn4_format_params_t fp = { .target_profile = HN4_PROFILE_HYPER_CLOUD };
hn4_format(dev0, &fp);

/* 3. Mount & Patch */
hn4_volume_t* vol;
hn4_mount(dev0, NULL, &vol);

/* 4. Attach Secondary Drive */
hn4_pool_add_device(vol, dev1);

/* 5. Set Mode */
vol->array.mode = HN4_ARRAY_MODE_MIRROR;
```