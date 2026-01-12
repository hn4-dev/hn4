<div align="center">

# HN4
### **The Post-POSIX Filesystem.** 

<!-- Badges -->
[![Status](https://img.shields.io/badge/Status-Final-orange?style=for-the-badge)](https://github.com/)
![Platform](https://img.shields.io/badge/platform-Bare%20Metal%20%7C%20Embedded%20%7C%20Kernel-orange.svg?style=for-the-badge)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue?style=for-the-badge)](https://opensource.org/licenses/Apache-2.0)

<br />

</div>

**HN4** is a high-velocity storage engine designed for the era of NVMe, ZNS, and direct-to-metal applications. It discards legacy assumptions (like spinning disks and inode trees) to achieve **O(1) data access** and **Zero-Copy latency**.

---

## 1. System Overview

HN4 is a freestanding, deterministic file system driver designed for high-performance and constrained computing environments. Unlike traditional file systems (EXT4, XFS, NTFS) that rely on B-Tree or extent-tree structures for block allocation, HN4 utilizes **Ballistic Addressing**.

This architecture eliminates tree traversal overhead, providing **amortized $O(1)$** allocation and lookup time complexity. The driver is designed to link directly into kernels, bootloaders, or bare-metal firmware without dependencies on `libc`, `malloc`, or POSIX threads.

### Key Capabilities
*   **Deterministic Latency:** Read/Write performance remains constant regardless of volume fragmentation or capacity utilization.
*   **Embedded to Enterprise:** Scales from 1MB micro-volumes (IoT/Embedded) to 18 Exabytes (Server/ZNS).
*   **Crash Consistency:** Uses an Epoch-based transaction ring to guarantee atomic state transitions without a journaling penalty.

---

## 2. Architectural Concepts

HN4 replaces standard file system nomenclature with physics-based allocation models to describe its deterministic behavior.

### 2.1. Ballistic Allocation (The Void Engine)
Instead of scanning allocation bitmaps for free blocks, HN4 calculates the physical location of data using a trajectory function.
*   **Mechanism:** Data placement is determined by a file's **Gravity Center ($G$)**, **Orbit Vector ($V$)**, and **Fractal Scale ($M$)**.
*   **Formula:**
    $$ LBA_{phys} = \left( G + (Sequence \times V) \right) \pmod{Capacity} $$
*   **Collision Resolution:** If the calculated block is occupied, the engine applies a **Gravity Assist** (bitwise XOR rotation) to the vector to select a deterministic alternative candidate ($k=0..12$).

### 2.2. Atomic Persistence (Shadow Hop)
HN4 enforces strict out-of-place update semantics.
*   **Mechanism:** Updates are never written in-place. New data is written to a calculated "Shadow" LBA.
*   **Commit:** A hardware barrier (FLUSH) is issued. Only after data persistence is confirmed is the file's **Anchor** (metadata node) updated in RAM to point to the new generation.
*   **Eclipse:** The old data block is atomically deallocated (bitmap cleared) after the metadata update is durable.

---

## 3. AI & Tensor Stream Architecture

HN4 includes a dedicated subsystem for Artificial Intelligence workloads (LLM Training/Inference) called the **Tensor Stream Layer**. This layer addresses the I/O bottlenecks inherent in loading multi-terabyte models across distributed topologies.

### 3.1. Tensor Virtualization
Large AI models are often sharded across multiple physical storage anchors to prevent allocation lock-ups. The Tensor Layer (`hn4_tensor.c`) virtualizes these distributed shards into a single, contiguous byte-addressable stream.
*   **Resonance Discovery:** The driver scans the Cortex metadata region using a **Bloom Filter**. It identifies all Anchors associated with a specific Model Tag (e.g., `model:llama3-70b`) in $O(N)$ linear time without directory traversal.
*   **Monotonic Ordering:** Shards are automatically sequenced based on their 128-bit Seed IDs, presenting a unified address space to the inference engine.

### 3.2. Topology Awareness (Path-Aware Striping)
In multi-GPU environments (e.g., HGX H100 pods), data locality is critical. HN4 implements **Path-Aware Striping**:
1.  **Topology Map:** At mount time, the driver queries the HAL for the system topology (NUMA nodes, PCIe switches).
2.  **Affinity Bias:** When allocating storage for a specific Tensor shard, the allocator checks the calling thread's hardware context (via `hn4_hal_get_calling_gpu_id`).
3.  **Local Placement:** The driver forces the allocation to occur in the physical Namespace or Zone physically closest to the requesting accelerator's PCIe root complex, minimizing QPI/UPI interconnect traffic during loading.

---

## 4. I/O Mechanics & Persistence Model

HN4 separates the persistence of **Data** (Payload) from the persistence of **Metadata** (Allocation Maps). This hybrid model ensures performance while maintaining consistency.

### 4.1. How Data is Written (The Shadow Hop)
HN4 updates data by shifting its ballistic trajectory rather than overwriting in place.

1.  **Trajectory Calculation:** To overwrite a logical block, the driver calculates a **new** physical location. This is done by incrementing the file's Generation counter, which alters the input variables for the ballistic equation ($G, V, k$).
2.  **Shadow Write:** The data is written to this new, empty physical block (the "Shadow" block).
3.  **Barrier:** The driver issues a hardware flush (FUA) to ensure the data is durable on the media.
4.  **Anchor Update:** Once durability is confirmed, the file's Anchor (metadata) is updated in **RAM** to reflect the new Generation count.
5.  **Eclipse:** The old physical block associated with the previous generation is immediately marked as free in the in-memory bitmap.

### 4.2. How Data is Read (Ballistic Projection)
Reading does not involve looking up block pointers in a table. It involves re-calculating where the data *must* be.

1.  **Projection:** The driver reads the file's Anchor to load the physics parameters ($G, V, M$) and the current Generation.
2.  **Calculation:** It computes the target LBA for the requested logical offset using the ballistic formula.
3.  **Shotgun Probe:** Because write collisions may have forced the data into a secondary orbit ($k>0$), the reader checks the calculated LBA.
4.  **Verification:** The driver reads the block and verifies:
    *   **CRC32:** Data integrity.
    *   **Well ID:** Ensures the block belongs to this file.
    *   **Sequence & Generation:** Ensures this is the specific byte-range and version requested, not stale data or a hash collision.
5.  **Iteration:** If validation fails (e.g., collision), the driver calculates the next orbital position ($k+1$) and repeats until the valid block is found.

### 4.3. How State is Saved (Global Persistence)
While data blocks are flushed immediately, the filesystem structure (Bitmaps and Anchors) is cached in RAM to maximize throughput. Global persistence occurs during **Sync** or **Unmount**:

1.  **Bitmap Flush:** The in-memory allocation bitmaps (Void Bitmap) and quality masks are written to their reserved regions on disk.
2.  **Cortex Flush:** Modified Anchors are written to the Cortex region.
3.  **Epoch Advance:** The global transaction counter is incremented and written to the Epoch Ring. This acts as the "Commit" timestamp.
4.  **Superblock Broadcast:** The volume Superblock is updated and broadcast to 4 physical locations (North, East, West, South) to ensure redundancy against physical media rot.

---

## 5. File Discovery (Namespace)

HN4 uses a flat, content-addressable metadata region called the **Cortex (D0)**. There are no directory trees.

### 5.1. The Resonance Scan
To locate a file, the driver performs a scan of the Cortex region:
1.  **Hashing:** The requested filename is hashed into a 64-bit Bloom Filter mask.
2.  **Scan:** The driver iterates through Anchor slots in the Cortex.
3.  **Filter Check:** It checks the Anchor's tag mask against the search hash.
    *   Mismatch: The Anchor is skipped (Cost: ~3 CPU cycles).
    *   Match: The driver performs a full string comparison to confirm identity.
4.  **Load:** Upon a match, the Anchor is loaded, initializing the ballistic variables ($G, V$) for I/O operations.

---

## 6. Storage Profiles

The physical layout of the volume adapts to the underlying media characteristics defined at format time.

| Profile | Target Hardware | Block Size | Optimization Strategy |
| :--- | :--- | :--- | :--- |
| **GENERIC** | NVMe / SATA SSD | 4KB | Standard Ballistic Allocation with full bitmap caching. |
| **PICO** | Microcontrollers | 512B | **Zero-RAM Mode.** Disables in-memory bitmaps. Uses direct flash reads for allocation checks to minimize heap usage (<2KB RAM). |
| **SYSTEM** | Bootloaders | 4KB | **The Launchpad.** Moves metadata to the physical center of the disk. Reserves the first 1GB for contiguous, linear boot files to optimize XIP. |
| **GAMING** | Workstations | 64KB | **Outer Rim Bias.** Prioritizes allocation on the outer tracks of HDDs (highest angular velocity) for low-latency asset streaming. |
| **AI** | H100 / Training Clusters | 64KB - 2MB | **Tensor Tunneling.** Uses large block alignment to match GPU page sizes. Enables path-aware striping based on topology maps. |
| **ARCHIVE** | Tape / SMR HDD | 64KB+ | **Linear Append.** Enforces strictly sequential writes to accommodate Shingled Magnetic Recording (SMR) zones. |

---

## 7. Developer Integration

To integrate HN4 into a kernel or firmware project, the following requirements must be met:

1.  **Compiler:** C99 or C11 compliant compiler (GCC, Clang, MSVC).
2.  **Endianness:** The driver handles endianness internally, but `hn4_endians.h` must be configured if the target is Big Endian.
3.  **Hardware Abstraction Layer (HAL):** You must implement `hn4_hal.c` to map the driver to your hardware. Required hooks:
    *   `hn4_hal_mem_alloc(size_t)` / `hn4_hal_mem_free(void*)`: Map to `kmalloc` or system heap.
    *   `hn4_hal_submit_io(...)`: Map to your block device driver (NVMe submission queue, AHCI, or SPI read/write).
    *   `hn4_hal_get_time_ns()`: A monotonic clock source.

---

## 8. Build & Usage Instructions

### Option A: Building a Simulation / Test Executable
For testing, fuzzing, or user-space utilities, compile all sources into a single binary.

```bash
# Linux / macOS (GCC or Clang)
gcc -std=c11 -O2 \
    src/*.c \
    test/sim_hal.c \
    test/main_test.c \
    -I include/ \
    -D_GNU_SOURCE \
    -o hn4_test

# Run the architectural verification suite
./hn4_test
```

### Option B: Building for Kernel / Bare Metal
When building for a kernel module or firmware, compile the core object file without linking the test harness.

```bash
# Compile Core Driver Object
gcc -std=c11 -O3 -ffreestanding -nostdlib \
    -c src/hn4_core.c \
    -c src/hn4_addr.c \
    -c src/hn4_allocator.c \
    -c src/hn4_read.c \
    -c src/hn4_write.c \
    -c src/hn4_tensor.c \
    -I include/ \
    -o hn4_driver.o

# Link hn4_driver.o with your OS kernel or firmware image
```

### Option C: Mounting a Volume (Code Example)
Once linked, the API is used as follows:

```c
#include "hn4.h"

void mount_example(hn4_hal_device_t* phys_dev) {
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t params = {0};

    // 1. Initialize HAL
    hn4_hal_init();

    // 2. Mount Volume
    // The driver automatically probes geometry and validates the superblock.
    hn4_result_t res = hn4_mount(phys_dev, &params, &vol);

    if (res == HN4_OK) {
        // 3. Resolve a File (Namespace Lookup)
        hn4_anchor_t file_anchor;
        res = hn4_ns_resolve(vol, "config/boot.cfg", &file_anchor);
        
        if (res == HN4_OK) {
            // 4. Read Data (Ballistic Read)
            char buffer[4096];
            // Read logical block 0
            hn4_read_block_atomic(vol, &file_anchor, 0, buffer, 4096);
        }
        
        // 5. Unmount (Flushes Bitmaps and Anchors to disk)
        hn4_unmount(vol);
    }
}
```
