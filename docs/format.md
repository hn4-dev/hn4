# HN4 FORMAT UTILITY SPECIFICATION
**Module:** `hn4_format.c`
**Version:** 9.1
**Type:** Design Specification

## 1. Module Scope

The `hn4_format` module is responsible for the initialization of physical storage media for the HN4 file system. It performs four critical operations in sequence:
1.  **Topology Verification:** Validates that the underlying hardware capabilities (HAL) align with the requested format profile.
2.  **Geometry Calculation:** Computes the precise Logical Block Address (LBA) boundaries for all metadata and data regions based on the device capacity.
3.  **Media Sanitization:** Issues hardware commands (`DISCARD` or `ZONE_RESET`) to clear existing data and reset the Flash Translation Layer (FTL).
4.  **Metadata Initialization:** Writes the initial Superblocks, Allocation Bitmaps, and Root Anchors to disk, establishing the file system genesis state.

---

## 2. Hardware Topology Validation

Before modifying the disk, the formatter validates the hardware flags provided by the Hardware Abstraction Layer (HAL). This prevents invalid configurations that could lead to data corruption or suboptimal performance.

### 2.1 Capability Conflicts
The driver enforces mutual exclusion rules for hardware types to ensure protocol compatibility:

| Flag A | Flag B | Result | Reason |
| :--- | :--- | :--- | :--- |
| `NVM` (Byte-Addressable) | `ROTATIONAL` (HDD) | **Error** | Physical contradiction. A device cannot possess both random-access byte-addressable characteristics and mechanical seek latency. |
| `NVM` (Byte-Addressable) | `ZNS` (Zoned Namespace) | **Error** | Protocol contradiction. NVM assumes unrestricted random access; ZNS enforces strict sequential append-only semantics. |

If a conflict is detected, the format operation aborts immediately with `HN4_ERR_PROFILE_MISMATCH`.

### 2.2 NVM Optimization Flag
If the device is validated as Non-Volatile Memory (NVM) and does not conflict with other flags, the formatter sets the `HN4_HW_NVM` bit in the Superblock `hw_caps_flags`. This signals the runtime allocator to use optimized lock-free atomic operations and bypass software ECC overhead during normal operation, significantly reducing latency.

---

## 3. Storage Profiles

The format utility adjusts the block size, alignment, and layout strategy based on the intended use case (`target_profile`) passed in the parameters.

| Profile | Block Size | Use Case | Implementation Details |
| :--- | :--- | :--- | :--- |
| **GENERIC** | 4 KB | SSD / NVMe | Standard balanced layout for general purpose computing. |
| **GAMING** | 16 KB | High Performance | Optimized for large sequential reads (assets). Sets aggressive read-ahead hints. |
| **AI** | 64 MB | Training Clusters | Enforces massive block sizes for Tensor Direct Memory Access (DMA). |
| **ARCHIVE** | 64 MB | HDD / Tape | Maximize sequential throughput for high-latency media. |
| **PICO** | 512 B | Embedded / IoT | Minimal memory footprint. Disables secondary overflow regions to save space. |
| **SYSTEM** | 4 KB | Boot Drives | Reserves the first 1GB of LBA space for bootloader binaries and kernel images. |
| **USB** | 64 KB | Portable Media | Optimized for lower IOPS devices. |

*Note: If the underlying device reports a Zoned Namespace (ZNS), the Logical Block Size is forcibly set to match the Physical Zone Size reported by the hardware to ensure write compatibility and prevent zone fragmentation.*

---

## 4. Volume Layout (Geometry)

The logical address space is partitioned into specific functional regions. All region start addresses are aligned to the larger of either the Profile Block Size or the physical sector size.

### 4.1 Address Map

| Region Name | Description | Size Calculation |
| :--- | :--- | :--- |
| **Superblock (Primary)** | Located at LBA 0. Contains geometry definitions, UUID, and state flags. | Fixed (8KB) |
| **Epoch Ring** | A circular buffer used to track transaction timestamps for crash recovery. | Fixed (1MB) or 2*BlockSize (Pico) |
| **Cortex (D0)** | The metadata region containing file definitions (Anchors). | ~2% of Capacity |
| **Bitmap (L3)** | The allocation map. Each bit represents one physical block. | Capacity / (BlockSize * 8) |
| **Quality Mask** | A secondary bitmap tracking the health status of physical blocks (2 bits per block). | Capacity / (BlockSize * 4) |
| **Data Region (D1)** | The primary area for file data storage. Accessed via calculated trajectory. | Remainder of Capacity |
| **Overflow Log (D1.5)** | A linear append area used when the primary Data Region is full (Horizon). | 10% of Capacity (min 4 blocks) |
| **Superblock Replicas** | Backup copies of the Superblock located at 33%, 66%, and 100% of the disk. | Fixed (8KB each) |

### 4.2 Region Calculation Algorithm
The layout engine calculates region sizes sequentially, ensuring alignment:
1.  **Block Size Resolution:** Determine final block size based on Profile and ZNS constraints.
2.  **Offset Accumulation:** Start at LBA 0 + SB_Size. Add regions sequentially (Epoch -> Cortex -> Bitmap -> QMask).
3.  **Flux Alignment:** Align the start of the Data Region (Flux) to the Profile's alignment target.
4.  **Tail Allocation:** Calculate Chronicle and Horizon regions relative to the end of the volume capacity to maximize contiguous data space.

---

## 5. Initialization Procedures

### 5.1 Device Sanitization
To ensure consistent performance and deterministic first-mount behavior, the device is sanitized before headers are written. This prevents "Ghost Data" from previous filesystems from confusing the recovery logic.
*   **SSD/NVMe:** Issues `HN4_IO_DISCARD` (TRIM) to the entire LBA range.
*   **ZNS:** Issues `HN4_IO_ZONE_RESET` to all zones sequentially.
*   **Explicit Zeroing:** Critical metadata areas (Epoch Ring, Cortex, Bitmaps) are explicitly overwritten with zeros (`0x00`) using standard write commands to ensure the media is physically cleared even if TRIM is lazy.

### 5.2 Bitmap Setup
The allocation bitmap is initialized to a "Logical Zero" state.
*   **Data:** All allocation bits are cleared (0).
*   **ECC:** The Hamming Code (ECC) byte for each word is calculated based on the zero value and written to the bitmap region. This ensures that the first read operation after format does not trigger a false "Bit Rot" error due to checksum mismatch.

### 5.3 Superblock Redundancy (Quorum)
To protect against physical media failure, the Superblock is written to four "Cardinal" locations:
1.  **North:** LBA 0 (Start of disk)
2.  **East:** ~33% Capacity
3.  **West:** ~66% Capacity
4.  **South:** End of Capacity (Minus 8KB Reservation)

The format operation returns success only if at least **North + 1 Mirror** (ZNS) or **3 Mirrors** (Standard) succeed. This "Quorum" logic ensures the volume is robust against sector failure immediately upon creation.

---

## 6. Virtualization Parameters

The format utility accepts parameters to support virtualization and special boot scenarios via the `hn4_format_params_t` structure.

*   **Clone UUID:** Allows forcing a specific 128-bit UUID onto the volume. Useful for RAID 1 setups or creating exact binary replicas for testing.
*   **Virtual Capacity:** Allows formatting a volume with a logical size smaller than the physical device size. Used for creating partitions or "short-stroking" a drive for performance.
*   **Root Permissions:** Sets the initial permission bits for the Root Anchor, allowing the creation of immutable "Read-Only" system images at format time.
*   **Wormhole Intent:** Flags the volume for special mounting behavior (e.g., `HN4_MNT_WORMHOLE`), enforcing strict flush semantics.

---

## 7. Error Handling

The module returns specific error codes to indicate failure modes:

*   `HN4_ERR_PROFILE_MISMATCH`: The requested profile (e.g., ARCHIVE) is incompatible with the detected hardware (e.g., NVM), or PICO profile used on a large volume.
*   `HN4_ERR_GEOMETRY`: The device capacity is too small or too large for the selected profile limits, or geometry calculation overflowed.
*   `HN4_ERR_ALIGNMENT_FAIL`: The geometry engine could not align regions to the required sector/block boundaries (common with 520-byte sectors or ZNS mismatches).
*   `HN4_ERR_HW_IO`: Hardware write failure during sanitization or superblock commit.
*   `HN4_ERR_ENOSPC`: Metadata regions consume the entire volume capacity, leaving no room for data.
