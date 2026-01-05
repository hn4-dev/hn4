# HN4 Storage Engine: Format Module Specification

**Status:** Production (v9.1)  
**Target:** Bare Metal / Kernel / Embedded  
**Architect:** Hydra-Nexus Engineering Team (Redmond Core)  
**Security Level:** Ring 0

---

## 1. Executive Summary

The `hn4_format` module acts as the **genesis engine** for HN4. Unlike traditional inode-based formatters (ext4, NTFS), HN4 does not write empty inode tables. Instead, it establishes the **Mathematical Manifold**—a set of geometric constants and boundaries that define the Ballistic Trajectory logic.

This module is responsible for:
1.  **Topology Validation:** Certifying physical media capabilities (NVM, ZNS, Rotational) and rejecting impossible configurations.
2.  **Geometry Convergence:** Calculating optimal region boundaries ($D_0, D_1, D_{1.5}, D_2$) to satisfy alignment requirements for atomic writes.
3.  **Root of Trust:** Establishing the 4-way replicated Superblock Quorum and the Epoch Ring.
4.  **Media sterilization:** issuing `DISCARD` / `ZONE_RESET` commands to ensure a deterministic initial state.

---

## 2. NVM.2 Protocol Integration

Version 9.1 introduces native support for **Byte-Addressable Non-Volatile Memory (NVM)**. This requires strict topology validation during the format phase to prevent data corruption in the Allocator's fast-path.

### 2.1 Topology Validation Logic
The formatter enforces physical invariants. If the HAL reports conflicting capabilities, the format operation aborts immediately with `HN4_ERR_PROFILE_MISMATCH`.

| Flag A | Flag B | Conflict? | Rationale |
| :--- | :--- | :---: | :--- |
| `HN4_HW_NVM` | `HN4_HW_ROTATIONAL` | **YES** | **Physics Violation:** A device cannot offer zero-latency byte-addressing (RAM) while having mechanical seek latency (HDD). |
| `HN4_HW_NVM` | `HN4_HW_ZNS_NATIVE` | **YES** | **Logic Violation:** NVM implies random write capability. ZNS enforces sequential append-only. The Allocator paths are mutually exclusive. |
| `HN4_HW_NVM` | `HN4_HW_GPU_DIRECT` | NO | Valid high-performance compute topology. |

### 2.2 Flag Inheritance
Upon successful validation, the `caps->hw_flags` (including `HN4_HW_NVM` bit 14) are serialized into `sb.info.hw_caps_flags`. This effectively **certificates** the volume for NVM optimizations (ECC bypass, Lock-Free atomic bitmap ops) used by the Allocator at runtime.

---

## 3. Format Profile Configuration Matrix

The layout engine adapts block sizes and region ratios based on the `target_profile`.

| Profile ID | Enum | Block Size ($BS$) | Target Media | Topology Characteristics |
| :--- | :--- | :---: | :--- | :--- |
| **0** | `HN4_PROFILE_GENERIC` | **4 KB** | NVMe SSD | Balanced layout. 50% Data, large metadata region. |
| **1** | `HN4_PROFILE_GAMING` | **4 KB** | NVMe (Gen4+) | Optimized for `HN4_ALLOC_LUDIC`. Max Capacity 16TB. |
| **2** | `HN4_PROFILE_AI` | **4 KB** | NVMe / NVM | Enforces Tensor Alignment (2MB) boundaries. Min Cap 512MB. |
| **3** | `HN4_PROFILE_ARCHIVE` | **64 KB** | HDD / Tape | **Inertial Damper Active.** High density. Aggressive V=1 enforcement. Max Cap 18 EB. |
| **4** | `HN4_PROFILE_PICO` | **512 B** | NOR/NAND Flash | Minimal footprint. No Horizon. Single-Superblock support (optional). |
| **5** | `HN4_PROFILE_SYSTEM` | **4 KB** | Boot/OS Drive | **Hot Zone Enforcement.** Reserves LBA 0-1GB for Bootloader. |

> **Note:** Virtual Containers (`HN4_HW_FILE_BACKED`) default to 4KB alignment regardless of profile to align with host page cache.

---

## 4. Disk Layout Specification (The Manifold)

HN4 partitions the linear address space into discrete functional regions. Alignment is enforced to `max(PhysicalSector, 4096)` or Zone Size.

### 4.1 Linear Address Map

```text
LBA 0 (North)
+-----------------------------------------------------------------------+
| SUPERBLOCK NORTH (8KB)                                                |  <-- Anchor Point 0
+-----------------------------------------------------------------------+
| EPOCH RING (1MB)                                                      |  <-- Time-Travel Journal
+-----------------------------------------------------------------------+
| CORTEX (D0)                                                           |
| [Anchor Table] - Hash-addressable metadata storage                    |
+-----------------------------------------------------------------------+
| ARMORED BITMAP (L3)                                                   |
| [128-bit Atomic Words] - ECC protected allocation map                 |
+-----------------------------------------------------------------------+
| QUALITY MASK (Silicon Cartography)                                    |
| [2-bit per block] - Tracks media health (Gold/Silver/Bronze/Toxic)    |
+-----------------------------------------------------------------------+
| FLUX MANIFOLD (D1)                                                    |
| [Ballistic Data Region]                                               |
|                                                                       |
| ... (Bulk Storage) ...                                                |
|                                                                       |
+-----------------------------------------------------------------------+
| HORIZON LOG (D1.5)                                                    |
| [Linear Append Buffer] - Overflow handler for hash collisions         |
+-----------------------------------------------------------------------+
| HYPER-STREAM (D2)                                                     |
| [Sequential Data] - Dedicated region for Archive/Stream objects       |
+-----------------------------------------------------------------------+
| ...                                                                   |
| SUPERBLOCK EAST / WEST / SOUTH (Replicas)                             |
+-----------------------------------------------------------------------+
End of Capacity
```

### 4.2 Critical Region Calculation (Algorithm)

The formatter uses an iterative convergence loop to calculate `lba_flux_start`. This is necessary because the size of the Bitmap depends on the size of the Flux region, but the Flux region size depends on where the Bitmap ends.

**Pseudocode:**
```c
// Iterative solver for layout
lba_flux = tentative_start;
for (pass = 0; pass < 64; pass++) {
    allocatable = total_blocks - lba_flux;
    bitmap_size = calculate_bitmap_size(allocatable);
    
    next_flux_start = lba_bitmap + bitmap_size + quality_mask_size;
    align_to_boundary(next_flux_start); // e.g. Zone Align
    
    if (next_flux_start == lba_flux) break; // Converged
    lba_flux = next_flux_start;
}
```

---

## 5. Implementation Details

### 5.1 The "Flash-Bang" Wipe
Before writing metadata, `hn4_format` issues `HN4_IO_DISCARD` or `HN4_IO_ZONE_RESET` to the entire device. This resets the Flash Translation Layer (FTL) on SSDs, restoring write performance.
*   **Chunking:** Operations are broken into 1GB chunks to prevent controller timeouts.
*   **Zeroing:** Critical metadata regions (Cortex, Epoch Ring) are explicitly zeroed (written with 0x00), not just discarded, to ensure deterministic reads on first mount.

### 5.2 Armored Bitmap Initialization
The bitmap is initialized to **Logical Zero** but **Physical Valid ECC**.
*   **Data:** `0x0000000000000000`
*   **ECC:** `_calc_ecc_hamming(0)` (Typically `0x00` or `0x7F` depending on polynomial).
*   **Why?** If we just left raw zeros without calculating ECC, the first read-modify-write operation would trigger a "Bit Rot" alarm because the checksum wouldn't match.

### 5.3 Superblock Quorum
HN4 does not rely on a single Superblock. It writes **4 Replicas** at mathematical cardinal points:
1.  **North:** LBA 0 (Start)
2.  **East:** 33% of Capacity
3.  **West:** 66% of Capacity
4.  **South:** End of Capacity - 8KB

**Commit Guarantee:** The format operation is considered successful only if **Quorum (3/4)** Superblocks are written successfully.

---

## 6. WORMHOLE Parameterization

The format parameters structure (`hn4_format_params_t`) supports the **WORMHOLE** virtualization protocol.

| Field | Type | Description |
| :--- | :--- | :--- |
| `clone_uuid` | `bool` | If true, forces the volume to adopt a specific UUID. Used for creating transparent overlays or mirrors. |
| `specific_uuid` | `u128` | The UUID to clone. |
| `override_capacity` | `u64` | **Virtual Geometry.** Forces the Superblock to report a capacity different from physical media. Used when formatting a RAM disk that will overlay a larger HDD. |
| `root_perms_or` | `u32` | Bitmask OR-ed into the Root Anchor permissions. Used to create immutable boot volumes at format time. |

---

## 7. Error Codes

| Error | Meaning | Mitigation |
| :--- | :--- | :--- |
| `HN4_ERR_PROFILE_MISMATCH` | Hardware flags contradict physics (e.g. NVM+HDD). | Check HAL initialization and capability reporting. |
| `HN4_ERR_GEOMETRY` | Device too large/small for requested profile. | Use appropriate profile (e.g. ARCHIVE for >16TB). |
| `HN4_ERR_ALIGNMENT_FAIL` | Layout solver failed to converge on aligned boundaries. | Verify Zone Size is power-of-two compatible. |
| `HN4_ERR_HW_IO` | Write failed or Quorum (3/4) not met. | Check physical media health. |