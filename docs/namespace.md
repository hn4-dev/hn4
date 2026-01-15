# HN4 Architecture: Namespace & Metadata Subsystem
**Status:** Implementation Standard v6.0
**Module:** Metadata Region (D0) & Semantic Layer
**Source:** `hn4_namespace.c`
**Scope:** Object Identity, Hash Resolution, and Workload Adaptation

---

## 1. Executive Summary: Flat Addressing Model

The **HN4 Namespace Subsystem** replaces the hierarchical directory tree (B-Tree/Extents) used by traditional filesystems with a **Flat, Mathematical Address Space**.

Every object is identified by a **128-bit Cryptographic Seed** (UUID). Location is not defined by a directory path string, but by the mathematical hash of this identity mapping to a slot in a linear metadata table. This architecture allows HN4 to adapt its resolution logic based on the workload type (e.g., AI Tensor vs. Sequential Stream).

### Visual Hierarchy
```text
+-----------------------------------------------------------------------+
|                       OS / USER SPACE                                 |
|   "open(file.txt)"   "open(tag:photo)"   "tensor_map(model.bin)"      |
+-----------------------------------+-----------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                        NAMESPACE RESOLVER                             |
|  [ URI Parser ] -> [ ID Hasher ] -> [ Bloom Filter ] -> [ Linear Scan ]|
+-----------------------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                        ANCHOR TABLE (D0 Region)                       |
|        Contiguous Physical Region acting as an On-Disk Hash Table     |
|  +-------------+  +-------------+  +-------------+  +-------------+   |
|  | Anchor #0   |  | Anchor #1   |  | Anchor #2   |  | Anchor #N   |   |
|  +-------------+  +-------------+  +-------------+  +-------------+   |
+-----------------------------------------------------------------------+
```

---

## 2. The Anchor Table (D0 Region)

The Anchor Table (formerly "Cortex") is the physical region allocated for Metadata. Unlike ext4 inodes which are scattered, the Anchor Table is a pre-allocated, contiguous **Linear Array**.

### 2.1 Physical Layout
*   **Anchor Size:** 128 Bytes (Fixed).
*   **Alignment:** 64-byte aligned (2x CPU Cache Lines).
*   **Density:** A 4KB physical page contains exactly **32 Anchors**.

```text
LBA 0       LBA 8KB     LBA 1MB         LBA 2MB (Table Start)
+-----------+-----------+---------------+----------------------------------+
| SB_NORTH  | EPOCH LOG |   RESERVED    | [ Anchor 0 ] [ Anchor 1 ] ...    |
+-----------+-----------+---------------+----------------------------------+
                                        <-- Hash Table Buckets Start Here
```

### 2.2 The Anchor Structure
The Anchor is the fundamental metadata record (Inode).

```c
// 128 Bytes Packed (hn4_anchor_t)
struct Anchor {
    u128 seed_id;        // 0x00: Immutable Identity (Hash Key)
    u128 public_id;      // 0x10: Mutable UUID (Renaming)
    u64  gravity_center; // 0x20: Physical Start LBA (G)
    u64  mass;           // 0x28: Logical File Size (Bytes)
    u64  data_class;     // 0x30: Flags (Type, Hints, State)
    u64  tag_filter;     // 0x38: 64-bit Bloom Filter
    u8   orbit_vector[6];// 0x40: Stride Vector (V)
    u16  fractal_scale;  // 0x46: Block Scale (2^M)
    u32  permissions;    // 0x48: Access Control Bitmask
    u32  sovereign_id;   // 0x4C: Owner Hash
    u64  mod_clock;      // 0x50: Modification Time (ns)
    u32  write_gen;      // 0x58: Consistency Counter
    u32  create_clock;   // 0x5C: Creation Time (Sec)
    u32  checksum;       // 0x60: Anchor Integrity CRC32C
    u8   inline[28];     // 0x64: Short Name or Tiny Data
};
```

---

## 3. The Resolution Pipeline

HN4 uses **Open Addressing Hash Table** logic directly on the storage media to locate files.

### 3.1 The Hash Function
The 128-bit Seed ID is mapped to a Slot Index using a hardware-optimized "Folded Multiply" hash (refer to `_ns_hash_uuid` in `hn4_namespace.c`).

```c
/* Implementation Logic */
uint64_t h = id.lo ^ id.hi;       // XOR Fold
h ^= (h >> 33);                   // Mixer 1
h *= 0xff51afd7ed558ccdULL;       // Prime Multiplier
h ^= (h >> 33);                   // Mixer 2
Slot_Index = h % Total_Slots;
```

### 3.2 Insertion Logic (Linear Probing)
The implementation uses **Linear Probing** to resolve hash collisions.

**Algorithm:**
1.  Calculate `Target_Slot = Hash(ID) % Capacity`.
2.  Check `Anchor[Target_Slot]`.
3.  If occupied (ID mismatch), increment slot index (`Target_Slot + 1`).
4.  Repeat until an Empty Slot (`ID == 0`) or a Tombstone is found.

### 3.3 Lookup Logic
The reader checks the Primary Slot, then scans linearly for a bounded distance (defined by `HN4_NS_MAX_PROBES = 1024`) to locate the record.

**Logic Flow (`_ns_scan_cortex_slot`):**
1.  Calculate Start Slot.
2.  Iterate up to 1024 slots.
3.  For each slot:
    *   Check `HN4_FLAG_VALID` and `HN4_FLAG_TOMBSTONE`.
    *   Compare `seed_id` with requested ID.
    *   Verify CRC32C integrity.
    *   If valid match found, return Anchor.
4.  If an empty slot (ID=0) is encountered, terminate search (Not Found).

---

## 4. Adaptive Workload Profiles

HN4 alters its I/O strategy based on the `Data_Class` flags stored in the Anchor.

### 4.1 AI Mode (`HN4_ALLOC_TENSOR`)
**Goal:** High-Throughput, Direct Access.
**Logic:**
1.  **Resolution:** `hn4_ns_gather_tensor_shards` scans the table for all Anchors matching a Model Tag.
2.  **Mapping:** Instead of reading data, it calculates the physical address ranges ($G, V, M$) for all shards.
3.  **Direct I/O:** The driver constructs a Scatter-Gather List (SGL) for Peer-to-Peer (P2P) DMA, allowing transfer directly to GPU memory (bypassing CPU buffers).

### 4.2 Gaming Mode (`HN4_ALLOC_LUDIC`)
**Goal:** Low Latency, Texture Streaming.
**Logic:**
1.  **Predictive Prefetch:** When a request for Block $N$ is received, the driver calculates the physical location of Block $N+1$ (the next LOD or asset chunk).
2.  **Deterministic Calc:** Because location is math-based ($LBA = G + N \times V$), prefetching requires zero metadata lookups.
3.  **Execution:** `hn4_hal_prefetch` is called to load data into the controller cache before the application requests it.

### 4.3 General Mode (`HN4_TYPE_UNSTRUCT`)
**Goal:** Search and Organization.
**Logic:**
1.  **Bloom Filter:** Uses the 64-bit `tag_filter` field to rapidly filter files (e.g., "Find all logs").
2.  **Virtualization:** The driver synthesizes a directory listing based on tag matches rather than physical folder structures.

---

## 5. Semantic Tagging (Bloom Filter)

HN4 supports O(1) attribute filtering using a bitwise Bloom Filter.

### 5.1 Bitmask Logic
Every Anchor contains a 64-bit `tag_filter`.
*   To tag a file (e.g., "Finance"):
    1.  Hash string "Finance" (FNV-1a).
    2.  Map hash to **3 distinct bit positions** ($0 \dots 63$).
    3.  Set bits in `tag_filter` (`_ns_generate_tag_mask`).

### 5.2 Query Engine
**Query:** `tag:Finance` AND `tag:2024`.

```c
/* Pseudocode */
uint64_t mask_finance = Hash("Finance"); // e.g. bits 2, 19, 44
uint64_t mask_2024    = Hash("2024");    // e.g. bits 5, 22, 60
uint64_t required     = mask_finance | mask_2024;

// Linear Scan of Metadata Region
if ((anchor.tag_filter & required) == required) {
    // Potential Match (Bloom Filter has false positives)
    // Perform string comparison on Extension Block to verify.
}
```

---

## 6. Extension Blocks (Linked Metadata)

The Anchor is fixed at 128 bytes. To support Long Filenames (>24 chars) and arbitrary tags, HN4 uses **Extension Blocks** stored in the Data Region (D1).

### 6.1 Chain Topology
If the `Inline_Buffer` contains a Pointer (LBA) and the `HN4_FLAG_EXTENDED` bit is set, it links to an external block.

```text
[ ANCHOR (D0) ]
   |
   +--> [ EXTENSION HEADER (D1) ]
           Magic: 0x4D455441 ("META")
           Type:  HN4_EXT_TYPE_LONGNAME
           Data:  "very_long_report_final_v2.pdf"
           Next:  LBA 500
             |
             +--> [ EXTENSION HEADER (D1) ]
                     Type:  HN4_EXT_TYPE_TAG
                     Data:  "Finance", "Q3", "Approved"
                     Next:  0 (End of Chain)
```

### 6.2 Safety Constraints
*   **Depth Limit:** `HN4_NS_MAX_EXT_DEPTH` (16 blocks). Prevents infinite loops.
*   **Validation:** Extension pointers must point to valid Data Region addresses (`_ns_verify_extension_ptr`).

---

## 7. URI Addressing Scheme

HN4 accesses objects via Uniform Resource Identifiers (URIs), parsed by `hn4_ns_resolve`.

### Syntax
`hn4://<VOLUME>/<SELECTOR>`

### Selectors

| Prefix | Type | Internal Logic | Performance |
|:---|:---|:---|:---|
| `id:` | **Identity** | Hashing -> Direct Slot Access | $O(1)$ |
| `tag:` | **Semantic** | Linear Scan + Bitwise AND | $O(N)$ (Metadata only) |
| `(none)` | **Name** | Linear Scan + `strcmp` | $O(N)$ (Metadata only) |

### Temporal Slicing
Suffixes allow accessing specific versions or timestamps.
*   `#time:<ns>`: Compares against `mod_clock`. Returns `HN4_ERR_TIME_PARADOX` if file is newer than requested time.
*   `#gen:<id>`: Compares against `write_gen`. Ensures atomic consistency with a specific transaction state.

---

## 8. Security Model

### 8.1 Deletion (Tombstones)
Deletion is a logical state change, not an immediate wipe.
*   **Flag:** `HN4_FLAG_TOMBSTONE` is set in `data_class`.
*   **Effect:** The file becomes invisible to standard lookups.
*   **Reclamation:** The Scavenger process (`_reap_tombstone`) scans for expired tombstones (older than 24 hours), zeroes the Anchor, and releases the bitmap bits.

### 8.2 Access Control
*   **Permissions:** A 32-bit mask (`anchor.permissions`) enforcing Read/Write/Exec.
*   **Immutability:** Setting `HN4_PERM_IMMUTABLE` creates a WORM (Write Once, Read Many) file. The driver physically rejects write commands to any handle associated with such an anchor.
