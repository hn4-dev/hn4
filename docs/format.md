# HN4 Format Utility Specification
**Module:** `hn4_format.c`
**Version:** 9.1
**Type:** Design Specification

## 1. Module Scope

The `hn4_format` module initializes the physical storage media for use with the HN4 file system. It performs four critical operations:
1.  **Topology Verification:** Validates that the underlying hardware capabilities match the requested format profile.
2.  **Geometry Calculation:** Computes the boundary addresses for all metadata and data regions based on the device capacity.
3.  **Media Sanitization:** Issues hardware commands (`DISCARD` or `ZONE_RESET`) to clear existing data and reset the Flash Translation Layer (FTL).
4.  **Metadata Initialization:** Writes the initial Superblocks, Allocation Bitmaps, and Root Anchors to disk.

---

## 2. Hardware Topology Validation

Before modifying the disk, the formatter validates the hardware flags provided by the Hardware Abstraction Layer (HAL). This prevents invalid configurations that could lead to data corruption.

### 2.1 Capability Conflicts
The driver enforces mutual exclusion rules for hardware types:

| Flag A | Flag B | Result | Reason |
| :--- | :--- | :--- | :--- |
| `NVM` (Byte-Addressable) | `ROTATIONAL` (HDD) | **Error** | Physical contradiction. A device cannot be both RAM-like and mechanical. |
| `NVM` (Byte-Addressable) | `ZNS` (Zoned Namespace) | **Error** | Protocol contradiction. NVM requires random access; ZNS enforces sequential append. |

If a conflict is detected, the format operation aborts with `HN4_ERR_PROFILE_MISMATCH`.

### 2.2 NVM Optimization Flag
If the device is validated as Non-Volatile Memory (NVM), the formatter sets the `HN4_HW_NVM` bit in the Superblock. This signals the runtime allocator to use optimized lock-free atomic operations and bypass software ECC overhead during normal operation.

---

## 3. Storage Profiles

The format utility adjusts the block size and layout strategy based on the intended use case (`target_profile`).

| Profile | Block Size | Use Case | Implementation Details |
| :--- | :--- | :--- | :--- |
| **GENERIC** | 4 KB | SSD / NVMe | Standard balanced layout. |
| **GAMING** | 4 KB | High Performance | Optimized for read-intensive workloads. Limits volume size to 16TB. |
| **AI** | 4 KB | Training Clusters | Enforces 2MB alignment boundaries for Tensor operations. |
| **ARCHIVE** | 64 KB | HDD / Tape | Sequential-write optimization for high-latency media. |
| **PICO** | 512 B | Embedded / IoT | Minimal memory footprint. Disables secondary overflow regions. |
| **SYSTEM** | 4 KB | Boot Drives | Reserves the first 1GB of LBA space for bootloader binaries. |

*Note: If the underlying device reports a Zoned Namespace (ZNS), the Block Size is forcibly set to the Zone Size reported by the hardware to ensure write compatibility.*

---

## 4. Volume Layout (Geometry)

The logical address space is partitioned into specific regions. All region start addresses are aligned to the larger of either 4KB or the physical sector size.

### 4.1 Address Map

| Region Name | Description |
| :--- | :--- |
| **Superblock (Primary)** | Located at LBA 0. Contains geometry definitions and UUID. |
| **Epoch Ring** | A circular buffer used to track transaction timestamps for crash recovery. |
| **Cortex (D0)** | The metadata region containing file definitions (Anchors). |
| **Bitmap (L3)** | The allocation map. Each bit represents one physical block. |
| **Quality Mask** | A secondary bitmap tracking the health status of physical blocks (2 bits per block). |
| **Data Region (D1)** | The primary area for file data storage. Accessed via calculated trajectory. |
| **Overflow Log (D1.5)** | A linear append area used when the primary Data Region is full. |
| **Sequential Region (D2)** | Reserved for streaming/archive workloads. |
| **Superblock Replicas** | Backup copies of the Superblock located at 33%, 66%, and 100% of the disk. |

### 4.2 Region Calculation Algorithm
The size of the Bitmap region depends on the size of the Data region, but the Data region size depends on where the Bitmap ends. The formatter uses an iterative loop to solve for these boundaries:
1.  Estimate the Data Region size.
2.  Calculate required Bitmap size.
3.  Adjust boundaries for alignment.
4.  Repeat until the start/end addresses stabilize (converge).

---

## 5. Initialization Procedures

### 5.1 Device Sanitization
To ensure consistent performance and deterministic first-mount behavior, the device is sanitized before headers are written.
*   **SSD/NVMe:** Issues `HN4_IO_DISCARD` (TRIM) to the entire LBA range.
*   **ZNS:** Issues `HN4_IO_ZONE_RESET` to all zones.
*   **Zeroing:** Critical metadata areas (Cortex, Epoch Ring) are explicitly overwritten with zeros (`0x00`) to prevent reading stale data from previous filesystems.

### 5.2 Bitmap Setup
The allocation bitmap is initialized to a "Logical Zero" state.
*   **Data:** All bits cleared (0).
*   **ECC:** The Hamming Code (ECC) byte for each word is calculated and written. This ensures that the first read operation after format does not trigger a false "Bit Rot" error due to checksum mismatch.

### 5.3 Superblock Redundancy (Quorum)
To protect against physical media failure, the Superblock is written to four locations:
1.  **LBA 0** (Start of disk)
2.  **33% Capacity**
3.  **66% Capacity**
4.  **End of Capacity** (Minus 8KB)

The format operation returns success only if at least 3 of the 4 writes succeed (Quorum).

---

## 6. Virtualization Parameters

The format utility accepts parameters to support virtualization and special boot scenarios via the `hn4_format_params_t` structure.

*   **Clone UUID:** Allows forcing a specific UUID onto the volume. Useful for RAID 1 setups or creating exact replicas.
*   **Virtual Capacity:** Allows formatting a volume with a logical size different from the physical device size. Used for sparse image creation or RAM disks overlaying physical storage.
*   **Root Permissions:** Sets the initial permission bits for the Root directory, allowing the creation of immutable "Read-Only" system images at format time.

---

## 7. Error Handling

The module returns specific error codes to indicate failure modes:

*   `HN4_ERR_PROFILE_MISMATCH`: The requested profile (e.g., ARCHIVE) is incompatible with the detected hardware (e.g., NVM).
*   `HN4_ERR_GEOMETRY`: The device capacity is too small or too large for the selected profile limits.
*   `HN4_ERR_ALIGNMENT_FAIL`: The geometry engine could not align regions to the required sector boundaries (common with 520-byte sectors).
*   `HN4_ERR_HW_IO`: Hardware write failure, or failure to achieve Superblock Quorum.