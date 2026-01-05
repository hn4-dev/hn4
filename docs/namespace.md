# HN4 Architecture: The Namespace Manifold
**Status:** Implementation Standard v6.0
**Module:** Cortex (D0) & Semantic Layer
**Source:** `hn4_namespace.c`
**Scope:** Identity, Resolution, Semantics, and Workload Adaptation

---

## 1. Executive Summary: The Post-POSIX Paradigm

The **HN4 Namespace Manifold** rejects the hierarchical directory tree (`/usr/bin/file`) used by traditional file systems. Instead, it utilizes a **Flat, Mathematical Address Space**.

Every object is identified by a **128-bit Cryptographic Seed**. Location is not defined by a path string, but by the mathematical hash of its identity. This architecture allows HN4 to behave as a **Chameleon**, altering its resolution logic based on the workload (AI Tensor vs. Game Asset).

### Visual Hierarchy
```text
+-----------------------------------------------------------------------+
|                       OS / USER SPACE                                 |
|   "open(file.txt)"   "open(tag:photo)"   "tensor_map(model.bin)"      |
+-----------------------------------+-----------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                        THE NAMESPACE MANIFOLD                         |
|  [ URI Parser ] -> [ ID Hasher ] -> [ Bloom Filter ] -> [ Resolver ]  |
+-----------------------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                           THE CORTEX (D0)                             |
|        Physical Region on Disk acting as a Hash Table                 |
|  +-------------+  +-------------+  +-------------+  +-------------+   |
|  | Anchor #0   |  | Anchor #1   |  | Anchor #2   |  | Anchor #N   |   |
|  +-------------+  +-------------+  +-------------+  +-------------+   |
+-----------------------------------------------------------------------+
```

---

## 2. The Cortex (D0 Region)

The Cortex is the physical region of the disk allocated for Metadata. Unlike ext4 inodes which are scattered, or NTFS MFT which fragments, the Cortex is a contiguous, pre-allocated **Linear Array**.

### 2.1 Physical Layout
*   **Anchor Size:** 128 Bytes (Fixed).
*   **Alignment:** 64-byte aligned (2x CPU Cache Lines).
*   **Density:** A 4KB physical page contains exactly **32 Anchors**.

```text
LBA 0       LBA 8KB     LBA 1MB         LBA 2MB (Cortex Start)
+-----------+-----------+---------------+----------------------------------+
| SB_NORTH  | EPOCH LOG |   RESERVED    | [ Anchor 0 ] [ Anchor 1 ] ...    |
+-----------+-----------+---------------+----------------------------------+
                                        <-- Hash Table Buckets Start Here
```

### 2.2 The Anchor Structure
The Anchor is the "Atom" of the file system.

```c
// 128 Bytes Packed
struct Anchor {
    u128 seed_id;        // 0x00: Immutable Identity (Key)
    u128 public_id;      // 0x10: Mutable UUID (Renaming)
    u64  gravity_center; // 0x20: Physical LBA of Data (G)
    u64  mass;           // 0x28: Logical Size
    u64  data_class;     // 0x30: Flags (AI/Game/Text)
    u64  tag_filter;     // 0x38: 64-bit Bloom Filter
    u8   orbit_vector[6];// 0x40: Ballistic Stride (V)
    u16  fractal_scale;  // 0x46: Block Size (2^M)
    u32  permissions;    // 0x48: Capability Bitmask
    u32  sovereign_id;   // 0x4C: Owner Hash
    u64  mod_clock;      // 0x50: Timestamp (ns)
    u32  write_gen;      // 0x58: Anti-Phantom Counter
    u32  create_clock;   // 0x5C: Creation Time
    u32  checksum;       // 0x60: Anchor Integrity CRC
    u8   inline[28];     // 0x64: Short Name or Tiny Data
};
```

---

## 3. The Resolution Pipeline (The $O(1)$ Guarantee)

How does HN4 find a file without a B-Tree? It uses **Hash Placement Logic**.

### 3.1 The Hash Function
We map the 128-bit ID to a Slot Index using a hardware-optimized "Folded Multiply" hash (implemented in `_ns_hash_id`).

```text
ID (128-bit)  -->  [ XOR Fold ] --> [ Multiply Constant ] --> [ Slot Index ]
                                           |
                                    0xff51afd7ed558ccd
```

### 3.2 Insertion Logic (Linear Probe)
The current implementation uses **Linear Probing** from the hashed slot index.

**Algorithm Visual:**
```text
INSERT ID "A" (Hash = 10)
1. Check Slot 10.
   [ Slot 10: Empty ] -> WRITE "A" to Slot 10. DONE.

INSERT ID "B" (Hash = 10)
1. Check Slot 10.
   [ Slot 10: Occupied by "A" ] -> COLLISION.
2. Probe Slot 11.
   [ Slot 11: Empty ] -> WRITE "B" to Slot 11. DONE.
```
*Result:* Items are clustered near their mathematical ideal location, maximizing cache locality.

### 3.3 Lookup Logic
Because deletions create "Tombstones" that might interrupt probe chains, the reader checks the Primary Slot, then scans linearly for a bounded distance (e.g., 1024 slots) to handle local clusters.

```text
LOOKUP ID "X"
1. Calculate Slot S = Hash(X) % Capacity.
2. Read 4KB Page at S (contains 32 anchors).
3. CPU Scan (SIMD):
   Compare(Anchor[0].ID, X)
   Compare(Anchor[1].ID, X)
   ...
   Compare(Anchor[31].ID, X)
4. Match Found? -> Return Anchor.
5. Else -> Fetch Next Page (Linear Probe).
```
**Latency:** 1 Disk Read (Best Case). Average $O(1)$ due to low load factor.

---

## 4. Workload Adaptation (The Chameleon)

HN4 changes its namespace behavior based on the `Data_Class` flag in the Anchor.

### 4.1 AI Mode (`TYPE_MATRIX`)
**Goal:** Maximum Throughput, Zero CPU Overhead.
**Scenario:** Training an LLM on 1TB of weights.

1.  **Resolution:** The Namespace resolves the ID to a **Ballistic Formula** ($G, V, M$).
2.  **The Tensor Tunnel:** Instead of reading data into Kernel RAM, the driver generates a **Scatter-Gather List (SGL)** of physical addresses based on the formula.
3.  **P2P DMA:** The NVMe controller is programmed to DMA these physical blocks **directly into GPU VRAM** via PCIe Peer-to-Peer (using `hn4_hal_map_p2p`).
4.  **CPU Action:** The CPU is bypassed completely after the initial metadata lookup.

### 4.2 Gaming Mode (`TYPE_LUDIC`)
**Goal:** Minimum Latency, Texture Streaming.
**Scenario:** Loading an Open World zone.

1.  **Ballistic Prefetch:** The Namespace sees the request for Texture LOD 0 (Low Res).
2.  **Logic:** Because the location of LOD 1 and LOD 2 is mathematically deterministic (Orbit $k=1, k=2$), the driver issues **Speculative Reads** for the high-res textures before the game engine asks for them.
3.  **Result:** Negative Latency. Data is in RAM before the request is made.

### 4.3 General/Human Mode (`TYPE_UNSTRUCT`)
**Goal:** Ease of use, Searchability.
**Scenario:** User searching for "Vacation Photos".

1.  **Bloom Filter:** The driver uses the **Semantic Tagging** engine (see below) to filter 100,000 files in microseconds.
2.  **Virtual Hierarchy:** The driver synthesizes a "Folder" view based on the tags, presenting a familiar interface to the OS Explorer via the POSIX Shim.

---

## 5. Semantic Tagging (The Bloom Filter)

HN4 replaces `mkdir` with `tag_add`.

### 5.1 The Bitmask Logic
Every Anchor contains a 64-bit `tag_filter`.
*   To tag a file "Finance":
    1.  Hash "Finance" -> 3 bit positions (e.g., 2, 19, 44).
    2.  Set bits in `tag_filter`.

### 5.2 The Query Engine
**Query:** `tag:Finance` AND `tag:2024`.

```text
Query "Finance": [0010...1000...10] (Mask A)
Query "2024":    [0000...1010...00] (Mask B)
Combined Mask:   (Mask A | Mask B)

SCAN LOOP (CPU SIMD):
   Load Anchor.tag_filter
   Result = (tag_filter & Combined_Mask)
   IF (Result == Combined_Mask) -> CANDIDATE
```

### 5.3 False Positive Defense (The Extension Check)
Bloom filters are probabilistic. A "Match" means "Maybe".
1.  **Bit Check:** Fast. Eliminates 99.9% of non-matches.
2.  **String Check:** If bits match, the driver traverses the **Extension Chain** (see below) to perform a `strcmp` on the actual tag strings stored on disk.
3.  **User View:** 100% Accuracy.

---

## 6. The Extension Manifold (Linked Lists in Space)

The Anchor is small (128 bytes). To support Long Filenames (>23 chars) and Unlimited Tags, HN4 uses **Extension Blocks**.

### 6.1 Chain Topology
If `Inline_Buffer` contains a Pointer (LBA), it links to D1 (Flux Region).

```text
[ ANCHOR (D0) ]
   |
   +--> [ EXT BLOCK 1 (D1) ]
           Type: LONGNAME
           Payload: "very_long_report_final_v2.pdf"
           Next: LBA 500
             |
             +--> [ EXT BLOCK 2 (D1) ]
                     Type: TAG
                     Payload: "Finance", "Q3", "Approved"
                     Next: 0 (End)
```

### 6.2 Safety Limits
*   **Depth Limit:** 16 Blocks. Prevents infinite loops (Ouroboros Attack).
*   **Magic Number:** Every extension block starts with `0x4D455441` ("META") to verify validity before parsing.

---

## 7. URI Addressing Scheme

HN4 standardizes file access via URIs, removing the dependency on mount points like `/mnt/sda1` or `C:\`.

### Scheme Syntax
`hn4://<VOLUME>/<SELECTOR>`

### Selector Types

| Prefix | Type | Example | Resolution Logic |
|:---|:---|:---|:---|
| `id:` | **Identity** | `id:a0b1...` | **Hash -> Slot.** Fastest. Used by System/DB. |
| `tag:` | **Semantic** | `tag:Photo` | **Scan D0 + Bloom Filter.** Returns Virtual Directory. |
| `(none)` | **Name** | `config.ini` | **Scan D0 + String Compare.** Human convenient. |

### Complex Query Example
`hn4://System/tag:Log+Error#time:2023-10`
*   **Volume:** System
*   **Filter:** Tags "Log" AND "Error".
*   **Slicing:** Jump to byte offset corresponding to timestamp `2023-10`.

---

## 8. Security Model

### 8.1 Tombstone Lifecycle (Deletion)
HN4 does not erase data immediately. It changes state.

```text
[ Active File ]  --delete()-->  [ Tombstone ]  --reaper()-->  [ Void ]
   (Visible)                   (Hidden/Undoable)             (Overwritten)
```
*   **Undelete:** Possible instantly by clearing the Tombstone flag (`HN4_FLAG_TOMBSTONE`).
*   **Purge:** The Reaper process scans D0 periodically, zeroing Tombstones and freeing Bitmap bits.

### 8.2 The Sovereign Key
*   **Permissions:** A 32-bit mask in the Anchor.
*   **Immutability:** `PERM_IMMUTABLE` creates a **WORM** (Write Once, Read Many) file. The driver physically rejects write commands to the logic layer, protecting against ransomware.