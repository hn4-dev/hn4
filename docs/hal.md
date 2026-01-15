# HN4 HARDWARE ABSTRACTION LAYER (HAL)
**Specification:** v3.1
**Module:** `hn4_hal.c` / `hn4_hal.h`
**Scope:** I/O Scheduling, Memory Management, CPU Primitives, and NVM Persistence

---

## 1. Executive Summary

The **HN4 HAL** serves as the translation layer between the abstract mathematical logic of the Void Engine and the physical reality of storage silicon. Unlike generic block layer shims, the HN4 HAL is **Topology Aware**. It explicitly exposes distinct behaviors for:

1.  **NVM (Non-Volatile Memory):** Byte-addressable persistence (DAX).
2.  **ZNS (Zoned Namespaces):** Append-only sequential constraints.
3.  **AI Accelerators:** PCIe Peer-to-Peer (P2P) DMA contexts.
4.  **Standard Block Devices:** SSD/HDD via NVMe/AHCI.

It abstracts the operating system (Linux/Windows/Bare Metal) while enforcing strict contracts regarding atomicity and persistence ordering.

---

## 2. The Persistence Model (NVM)

The most critical function of the HAL is bridging the gap between **Volatile CPU Cache** and **Persistent Media**.

### 2.1 The Durability Domain
On NVM platforms (Optane, CXL.mem), data is considered "Persisted" only when it reaches the **ADR (Asynchronous DRAM Refresh)** domain.

The function `hn4_hal_nvm_persist(ptr, size)` implements the architecture-specific barriers required to flush CPU write buffers:

| Architecture | Instruction Sequence | Semantics |
| :--- | :--- | :--- |
| **x86_64** | `CLFLUSHOPT` / `CLWB` + `SFENCE` | Ejects cache line to memory controller. `SFENCE` ensures global visibility order. |
| **ARM64** | `DC CVAP` + `DSB ISH` | Clean Data Cache to Point of Persistence. `DSB` ensures completion. |
| **Legacy** | `Atomic Fence` | Fallback for non-NVM hardware (relies on block flush). |

**Safety Invariant:** The HAL guarantees that upon return of `hn4_hal_nvm_persist`, the data is safe against power loss, provided the platform hardware adheres to JEDEC NVDIMM standards.

---

## 3. I/O Architecture

The HAL presents an asynchronous submission model with synchronous wrappers.

### 3.1 Operations (`hn4_io_req_t`)

| Op Code | Description | Constraints |
| :--- | :--- | :--- |
| `HN4_IO_READ` | DMA Read | Must be Sector Aligned. |
| `HN4_IO_WRITE` | DMA Write | Must be Sector Aligned. |
| `HN4_IO_FLUSH` | Hardware Barrier | Triggers `NVMe Flush` or `FUA` (Force Unit Access). |
| `HN4_IO_DISCARD`| Trim / Deallocate | Hint to FTL to erase blocks. |
| `HN4_IO_ZONE_APPEND` | ZNS Append | Writer does not specify LBA; Drive returns `result_lba`. |
| `HN4_IO_ZONE_RESET` | ZNS Reset | Sets Zone Write Pointer to 0. Destructive. |

### 3.2 The Safety Chunker (`sync_io_large`)
The kernel stack often limits DMA transfer sizes (e.g., 2MB or 4MB). The HAL implements a "Safety Chunker" for large operations (e.g., Formatting, Scavenging).

*   **Logic:** Breaks large requests into hardware-friendly chunks (e.g., 2GB or Max_Transfer).
*   **Anti-Deadlock:** Validates that `Chunk_Size >= Block_Size`.
*   **Yielding:** Calls `HN4_YIELD()` between chunks to prevent watchdog timeouts on single-threaded embedded controllers.

---

## 4. Memory Management

HN4 requires strict alignment for **128-bit CAS** operations and SIMD processing. The HAL allocator is not a simple `malloc` wrapper.

### 4.1 Alignment Contract
*   **Base Alignment:** 128 Bytes.
*   **Reasoning:** Ensures structures like `hn4_anchor_t` (128 bytes) never straddle cache lines, ensuring atomicity during updates.
*   **Padding:** Headers are padded to ensure the payload starts on a cache-line boundary.

### 4.2 Guard Rails
*   **Headers:** Every allocation creates a hidden header containing a Magic Number (`0x484E3421`).
*   **Overflow Check:** Validates `Size + Overhead < SIZE_MAX` before allocation.
*   **Poisoning:** `hn4_hal_mem_free` overwrites the Magic Number with `0xDEADBEEF` to detect Double-Free or Use-After-Free bugs immediately.

---

## 5. Zoned Namespace (ZNS) Abstraction

ZNS drives require sequential writes. Random writes cause I/O errors. The HAL provides primitives to manage this constraint.

### 5.1 Zone Append Emulation
For hardware that supports it, `HN4_IO_ZONE_APPEND` is passed through.
For simulation/testing, the HAL maintains an atomic software Write Pointer (`_zns_zone_ptrs`).

*   **Logic:** `Atomic_Add(Zone_Pointer, Length)`.
*   **Boundary Check:** If `Pointer + Length > Zone_Capacity`, return `HN4_ERR_ZONE_FULL`.

### 5.2 Zone Reset
Issues the physical reset command to the drive. In simulation mode, atomically resets the software pointer to 0.

---

## 6. AI Topology Services

To support `HN4_PROFILE_AI`, the HAL exposes the physical relationship between Storage and Compute.

### 6.1 GPU Context Identity
`hn4_hal_get_calling_gpu_id()`
*   **Mechanism:** Uses Thread Local Storage (TLS) to identify if the calling thread is bound to a specific Accelerator Context (CUDA Stream / ROCm Queue).
*   **Return:** The PCI ID or Index of the GPU.

### 6.2 Topology Mapping
`hn4_hal_get_topology_data()`
*   **Purpose:** Returns a map of `{GPU_ID, Weight, LBA_Start, LBA_Len}`.
*   **Usage:** The Allocator uses this to bias allocations. If GPU #1 requests storage, the Allocator restricts selection to the LBA range physically closest to GPU #1 (e.g., same PCIe Switch).

---

## 7. Concurrency Primitives

The HAL provides architecture-agnostic synchronization.

*   **Spinlocks:** Uses `atomic_flag` (TAS) with exponential backoff (`HN4_YIELD` / `_mm_pause`).
*   **Barriers:** `atomic_thread_fence` mapped to hardware memory barriers (`mfence`, `dmb`).

---

## 8. Telemetry & Environment

*   **Time:** `hn4_hal_get_time_ns()` provides monotonic time for timestamps and performance profiling.
*   **Entropy:** `hn4_hal_get_random_u64()` provides fast pseudo-random numbers for the Monte Carlo allocator logic.
*   **Thermals:** `hn4_hal_get_temperature()` allows the filesystem to throttle I/O if the physical media is overheating (e.g., >85°C).

---

## 9. Integration Checklist

When porting HN4 to a new platform (e.g., a new RTOS or FPGA controller), implementers must:

1.  **Map Memory:** Wire `hn4_hal_mem_alloc` to the system heap (DMA-capable).
2.  **Map Persist:** Implement `hn4_hal_nvm_persist` using correct assembly for the CPU.
3.  **Map Block I/O:** Connect `submit_io` to the block driver's request queue.
4.  **Define Capabilities:** Populate `hn4_hal_caps_t` with correct Sector/Block/Zone sizes.