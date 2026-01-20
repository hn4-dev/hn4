# HN4 STORAGE ENGINE: HELIX-D SUBSYSTEM
**File:** `hn4_helix_d.md`
**Version:** 3.1 (Architecture Specification & Abstract)
**Status:** **RATIFIED**
**Author:** Core Systems Engineering
**System:** Hydra-Nexus 4 (HN4) Kernel Module

---

## 1. Abstract

**Helix-D: A Deterministic Spatial Array Router for Exascale Storage Topologies**

*By The Hydra-Nexus Research Group*

This paper introduces Helix-D, the topology management subsystem of the Hydra-Nexus 4 (HN4) file system. As storage capacities approach the Quettabyte horizon, traditional logical volume managers (LVM) and RAID controllers introduce unacceptable latency and abstraction overhead. Helix-D eliminates the "Fake Block Device" layer by integrating topology awareness directly into the file system's addressing logic.

We present a stateless, algorithmic router that maps logical file extents to physical devices using a "Ballistic Trajectory" function, rather than static lookup tables. This approach reduces metadata memory footprint to $O(1)$ regardless of volume size. Helix-D implements a novel dual-parity erasure coding scheme (The Constellation) based on Galois Field arithmetic ($GF(2^8)$), optimized for SIMD execution.

Crucially, Helix-D solves the RAID "Write Hole" phenomenon without battery-backed NVRAM by leveraging the HN4 Chronicle—a cryptographically linked, circular intent log. We demonstrate that this integrated approach provides atomic consistency guarantees for distributed writes while maintaining linear scalability in sharded configurations and robust fault tolerance in parity modes. This architecture unifies the performance of striping with the resilience of erasure coding, tailored specifically for hyper-converged and AI-centric workloads.

---

## 2. Executive Summary: The Anti-RAID

**Helix-D** (The Spatial Array Router) is the distributed storage topology manager for HN4. While it fulfills the role of redundancy and striping, **it is not RAID**.

Traditional RAID sits *below* the filesystem, presenting a "Fake Block Device." The filesystem is unaware of the physical topology.
**Helix-D** sits *inside* the filesystem (Layer 3). The Allocator, Scavenger, and IO Path are topology-aware.

### Core Distinctions
| Feature | Legacy RAID | HN4 Helix-D |
| :--- | :--- | :--- |
| **Abstraction** | "Dumb" Logical Block Device | Semantic "Hyper-Cloud" Volume |
| **Addressing** | LBA Mapping Table (RAM Heavy) | **Ballistic Calculation** (CPU Heavy, Zero RAM) |
| **Write Hole** | Requires Battery/NVRAM | Solved via **Intent Logging (Wormhole)** |
| **Recovery** | Blind Sector Rebuild | **Auto-Medic** (Context-Aware Healing) |
| **Expansion** | Painful Reshape | **Osteoplasty** (Background Vector Migration) |

---

## 3. Architecture & The Spatial Router

Helix-D operates as a **Stateless Router**. It does not maintain a map of where every block is located. Instead, it calculates where a block *must be* based on the file's **Identity Seed** and **Trajectory Vector**.

### 3.1 The Dispatch Pipeline
```ascii
[ Logical Request (File ID, Block N) ]
          |
          v
[ BALLISTIC ENGINE (Layer 2) ]
   Calculates -> Logical LBA (e.g., 1024)
          |
          v
[ SPATIAL ROUTER (Layer 3 - Helix) ]
   Input: Logical LBA + Array Mode
   Logic: Map LBA -> {Device_Index, Physical_LBA}
          |
          +--- Mode: SHARD  ---> Hash(ID) % N
          +--- Mode: MIRROR ---> Broadcast to All
          +--- Mode: PARITY ---> Striping + Galois Calc
          |
          v
[ HAL (Layer 1) ]
   Dispatches NVMe/ZNS Commands
```

### 3.2 Concurrency Model
Helix-D utilizes **Fine-Grained Row Locking**.
*   The address space is hashed into **64 Shard Locks** (`HN4_CORTEX_SHARDS`).
*   Concurrent writes to different rows proceed in parallel.
*   Writes to the *same* row are serialized to prevent RMW (Read-Modify-Write) races.

---

## 4. Topology Modes

Helix-D supports four distinct topological configurations, defined in the Superblock.

### Mode 0: Singularity (Passthrough)
*   **Description:** 1:1 Mapping. No redundancy.
*   **Use Case:** Single Drive, PICO profiles.
*   **Math:** `Phys_LBA = Logical_LBA`

### Mode 1: Entanglement (Mirror)
*   **Description:** N-Way Active/Active Mirroring.
*   **Write Policy:** **Strict Consensus**. Data is written to all online devices. If *any* device fails, the volume is marked **DEGRADED**.
*   **Read Policy:** **Divergence Priority**. Reads prefer Device 0. If Dev 0 is busy/offline, it rotates.
*   **Use Case:** System Boot Volumes, High-Safety Database Logs.

### Mode 2: Ballistic Sharding
*   **Description:** Deterministic distribution based on File ID.
*   **Logic:** `Shard_ID = Hash(Anchor.Seed_ID) % Device_Count`
*   **Benefit:** Zero-coordination throughput. File A goes to Drive 1, File B goes to Drive 2. No striping overhead.
*   **Fault Tolerance:** None. Loss of one shard = Loss of files mapped to it.
*   **Use Case:** AI Training Data, Temp Scratch, Gaming Assets.

### Mode 3: The Constellation (Parity / "RAID-6")
*   **Description:** Dual-Parity Striping ($N+2$).
*   **Layout:** Left-Symmetric Rotation.
*   **Stripe Unit:** 64KB (128 Sectors).
*   **Fault Tolerance:** Can survive **any 2** simultaneous drive failures.
*   **Use Case:** Hyper-Cloud Storage, Archival.

---

## 5. The Mathematics of Parity (Mode 3)

Helix-D implements a **Galois Field $GF(2^8)$** erasure coding engine. It maintains two parity syndromes, $P$ and $Q$.

### 5.1 The Field Generator
*   **Polynomial:** $x^8 + x^4 + x^3 + x^2 + 1$ (0x11D).
*   **Generator ($g$):** The primitive element used for Q-syndrome coefficients.

### 5.2 The Equations
For a stripe with data blocks $D_0, D_1, ... D_n$:

1.  **P-Parity (XOR):**
    $$ P = \bigoplus_{i=0}^{n} D_i $$

2.  **Q-Parity (Weighted Sum):**
    $$ Q = \bigoplus_{i=0}^{n} (g^{i} \cdot D_i) $$
    *(Where $\cdot$ is Galois Multiplication and $\bigoplus$ is XOR)*

### 5.3 Delta Updates (RMW Optimization)
To avoid reading the entire stripe for small writes, Helix-D uses Delta updates:
1.  Read `Old_Data`.
2.  Compute `Delta = Old_Data ^ New_Data`.
3.  `New_P = Old_P ^ Delta`.
4.  `New_Q = Old_Q ^ (Delta * g^Col_Index)`.

---

## 6. Recovery & Reconstruction

Helix-D includes an integrated solver for on-the-fly recovery during reads.

### 6.1 Failure Scenarios & Solutions

| Failure Type | Missing Drives | Solution Logic |
| :--- | :--- | :--- |
| **Single Data** | $D_x$ | $D_x = P \oplus \sum(Survivors)$ (Cheap XOR) |
| **Single P** | $P$ | Recompute from Data (XOR) |
| **Single Q** | $Q$ | Recompute from Data (Galois) |
| **Dual (Data + P)** | $D_x, P$ | Solve $Q$ equation for $D_x$ (Galois Inv) |
| **Dual (Data + Q)** | $D_x, Q$ | Solve $P$ equation for $D_x$ (XOR) |
| **Dual Data** | $D_x, D_y$ | **Algebraic Solve:** System of 2 equations ($P, Q$) with 2 unknowns. Requires Galois Inversion. |

### 6.2 Auto-Medic (Self-Healing)
When a read fails (due to IO error or Bit Rot detected by CRC):
1.  The router freezes the request.
2.  It triggers **Reconstruction** using surviving drives/parity.
3.  It returns the reconstructed data to the user (Latency penalty, but no error).
4.  **Asynchronously:** It issues a **Write** to the failed sector with the good data (**Scrubbing**).

---

## 7. The "Write Hole" Solution

Traditional RAID-5/6 suffers from the "Write Hole": if power fails after writing Data but before writing Parity, the stripe is inconsistent.

### The HN4 Solution: Audit Log Intent (Wormhole)
Helix-D leverages the **Chronicle** (Journal) before every write.

1.  **Phase A (Intent):**
    *   Router calculates target LBA.
    *   Router appends `OP_WORMHOLE` to the Chronicle.
    *   Payload: `Target_LBA` + `Health_Map` (Who is alive).
    *   **BARRIER (FLUSH):** Wait for Log persistence.

2.  **Phase B (Action):**
    *   Write Data.
    *   Write P.
    *   Write Q.

3.  **Recovery:**
    *   On mount, Scavenger scans the tail of the Chronicle.
    *   If it finds an `OP_WORMHOLE` that lacks a matching commit/next entry, it marks that stripe as **Dirty**.
    *   A background process scrubs (recalculates parity) for that stripe.

---

## 8. Performance & Throughput

### 8.1 Overhead Analysis

| Mode | Read Overhead | Write Overhead | Capacity Eff. |
| :--- | :--- | :--- | :--- |
| **Shard** | 1x (Native) | 1x (Native) | 100% |
| **Mirror** | 1x (Load Balanced) | 2x (Write to both) | 50% |
| **Parity** | 1x (Native) | 2x (Read Old + Write New) | $(N-2)/N$ |

### 8.2 Scalability Graph

```ascii
Throughput (MB/s)
   ^
   |           / (Shard Mode - Linear Scaling)
   |         /
   |       /
   |     /
   |   /_______ (Parity Mode - IOPS Limited by RMW)
   |  /
   | /
   +---------------------> Drive Count
```

*   **Shard Mode:** Linearly scales bandwidth and IOPS with drive count.
*   **Parity Mode:** High streaming bandwidth (full stripe writes), but random write IOPS are limited by the Read-Modify-Write cycle (requiring 2 reads + 2 writes per logical write).

---

## 9. Technical Limits

1.  **Max Devices:** 16 per Volume (`HN4_MAX_ARRAY_DEVICES`).
    *   *Reason:* Memory footprint of `hn4_volume_t` and stack buffer limits for reconstruction.
2.  **Max Stripe Width:** 16 (matches device count).
3.  **Stripe Unit Size:** Fixed at 64KB (128 Sectors).
    *   *Reason:* Optimized for average 4KB-16KB IO workloads while keeping parity calculation efficient.
4.  **CPU Cost:**
    *   XOR (P-Parity): Negligible (SIMD optimized).
    *   Galois (Q-Parity): ~2-3x cost of XOR. Uses Look-up Tables (LUT) for $O(1)$ multiplication.

***
*Confidential - Internal Documentation*