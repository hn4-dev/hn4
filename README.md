<div align="center">

# HN4
### **The Post-POSIX Filesystem.** 

<!-- Badges -->
[![Status](https://img.shields.io/badge/Status-Final-orange?style=for-the-badge)](https://github.com/)
![Platform](https://img.shields.io/badge/platform-Bare%20Metal%20%7C%20Embedded%20%7C%20Kernel-orange.svg?style=for-the-badge)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue?style=for-the-badge)](https://opensource.org/licenses/Apache-2.0)

<br />

</div>

**HN4** is a high-velocity storage engine designed for the modern era of NVMe, ZNS, and direct-to-metal applications. It discards legacy assumptions—like spinning magnetic platters and complex directory trees—to achieve **O(1) data access** and **Zero-Copy latency**.

If standard file systems are like filing cabinets, HN4 is a teleportation device.

---

## 1. System Overview

HN4 is a freestanding, deterministic file system driver designed for high-performance and constrained computing environments. Unlike traditional file systems (EXT4, XFS, NTFS) that rely on heavy structures like B-Trees (think of a complex library index cards) to find empty space, HN4 uses **Ballistic Addressing**.

This architecture eliminates the need to "search" for space, providing **amortized $O(1)$** performance. This means the time it takes to read or write a file is mathematically constant, regardless of how full or fragmented the drive is.

The driver is designed to link directly into kernels, bootloaders, or bare-metal firmware without dependencies on standard libraries like `libc`, `malloc`, or POSIX threads.

### Key Capabilities
*   **Deterministic Latency:** Performance never degrades. A 99% full drive performs exactly the same as an empty one.
*   **Embedded to Enterprise:** Scales effortlessly from 1MB micro-volumes (IoT/Embedded) to 18 Exabytes (Server/ZNS).
*   **Crash Consistency:** Uses an Epoch-based transaction ring to guarantee atomic state transitions without the performance penalty of a journal.

---

## 2. Architectural Concepts & The Math

HN4 replaces standard file system jargon with physics-based allocation models. Here is how the math actually works.

### 2.1. Ballistic Allocation (The Void Engine)

**The Problem:** Traditional file systems use a "Bitmap"—a giant map of every block on the disk—to find free space. When writing a file, the computer has to scan this map to find open spots. As the drive fills up, this scan takes longer.

**The HN4 Solution:** Instead of looking for space, HN4 **calculates** where the data should go using a trajectory function. It doesn't ask "Where is space?" It says, "Data, go *there*."

*   **Mechanism:** Data placement is determined by three variables:
    1.  **Gravity Center ($G$):** The starting point of the file on the disk.
    2.  **Orbit Vector ($V$):** The "velocity" or jump size between file blocks.
    3.  **Fractal Scale ($M$):** The size of the chunks being written.

*   **The Formula:**
    $$ LBA_{phys} = \left( G + (Sequence \times V) \right) \pmod{Capacity} $$

    > **For Noobs:** Imagine a clock face (the Capacity).
    > *   **$G$** is where you start (e.g., 12 o'clock).
    > *   **$Sequence$** is the page number of the file you are writing (Page 0, Page 1, Page 2...).
    > *   **$V$** is how many hours you jump for every page.
    >
    > If $V=1$, you write to 12, 1, 2, 3 (Sequential).
    > If $V=5$, you write to 12, 5, 10, 3 (Scattered/Ballistic).
    >
    > Because this is pure math, the CPU knows exactly where Page 500 is instantly, without looking it up in a list.

*   **Collision Resolution:** What if the calculated spot is already taken? The engine applies a **Gravity Assist**. It bitwise rotates the vector (changes the angle of approach) to find a deterministic alternative candidate ($k=0..12$).

### 2.2. Atomic Persistence (Shadow Hop)
HN4 enforces strict out-of-place update semantics. This prevents data corruption during power loss.

*   **Mechanism:** We never overwrite data in place. If you edit a file, the new data is written to a calculated "Shadow" LBA (a new location).
*   **Commit:** A hardware barrier (FLUSH) ensures the data is physically burned onto the chip. Only *after* safety is confirmed does the file's **Anchor** (metadata node) update in RAM to point to the new location.
*   **Eclipse:** The old data block is atomically marked as free (eclipsed) only after the new data is safe.

---

## 3. AI & Tensor Stream Architecture

HN4 includes a dedicated subsystem for Artificial Intelligence workloads (LLM Training/Inference) called the **Tensor Stream Layer**. This addresses the massive I/O bottlenecks inherent in loading multi-terabyte models.

### 3.1. Tensor Virtualization
Large AI models are often cut into pieces (shards) across many drives to prevent locking up the system. The Tensor Layer (`hn4_tensor.c`) creates an illusion: it virtualizes these distributed pieces into a single, continuous stream of bytes.
*   **Resonance Discovery:** The driver scans the **Cortex** (metadata region) using a **Bloom Filter** (a probability-based search). It finds all parts of a model (e.g., `model:llama3-70b`) in linear time without digging through folders.
*   **Monotonic Ordering:** Shards are automatically sequenced, presenting a unified address space to the AI inference engine.

### 3.2. Topology Awareness (Path-Aware Striping)
In multi-GPU environments (like HGX H100 pods), distance matters. Sending data across the motherboard is slow; keeping it local is fast. HN4 implements **Path-Aware Striping**:
1.  **Topology Map:** At mount time, the driver maps the physical hardware layout (NUMA nodes, PCIe switches).
2.  **Affinity Bias:** When allocating storage for a Tensor shard, the allocator checks *which* GPU is asking for the data (via `hn4_hal_get_calling_gpu_id`).
3.  **Local Placement:** The driver forces the data to be stored on the physical flash chips closest to that specific GPU, ensuring maximum speed.

---

## 4. I/O Mechanics & Persistence Model

HN4 separates the persistence of **Data** (Payload) from the persistence of **Metadata** (The Maps). This hybrid model ensures extreme performance while maintaining safety.

### 4.1. How Data is Written (The Shadow Hop)
HN4 updates data by shifting its ballistic trajectory rather than overwriting.

1.  **Trajectory Calculation:** To overwrite a block, the driver increments the file's "Generation" counter. This changes the math variables ($G, V, k$), resulting in a totally new physical location calculation.
2.  **Shadow Write:** The data is written to this new, empty physical block.
3.  **Barrier:** The driver issues a hardware flush (FUA) to ensure durability.
4.  **Anchor Update:** Once durable, the file's Anchor is updated in **RAM** to reflect the new Generation.
5.  **Eclipse:** The old physical block is immediately freed in the in-memory bitmap.

### 4.2. How Data is Read (Ballistic Projection)
Reading does not involve looking up block pointers in a giant table. It involves re-calculating where the data *must* be.

1.  **Projection:** The driver reads the file's Anchor to get the physics parameters ($G, V, M$).
2.  **Calculation:** It computes the target LBA using the ballistic formula.
3.  **Shotgun Probe:** Because write collisions might have forced data into a secondary orbit, the reader checks the calculated LBA.
4.  **Verification:** The driver reads the block and verifies it:
    *   **CRC32:** Is the data intact?
    *   **Well ID:** Does this block actually belong to this file?
    *   **Sequence & Generation:** Is this the specific version we asked for?
5.  **Iteration:** If verification fails (e.g., we found an old version or a different file), the driver calculates the next orbital position and tries again.

### 4.3. How State is Saved (Global Persistence)
While data blocks are flushed immediately, the filesystem structure is cached in RAM. Global persistence happens during **Sync** or **Unmount**:

1.  **Bitmap Flush:** The in-memory allocation maps are written to disk.
2.  **Cortex Flush:** Modified Anchors are written to the Cortex region.
3.  **Epoch Advance:** The global transaction counter is incremented and written to the Epoch Ring. This acts as the "Commit" timestamp.
4.  **Superblock Broadcast:** The volume Superblock is updated and sent to 4 physical locations (North, East, West, South) to survive physical media rot.

---

## 5. File Discovery (Namespace)

HN4 uses a flat, content-addressable metadata region called the **Cortex (D0)**. There are no folders, no trees, no complexity.

### 5.1. The Resonance Scan
To find a file:
1.  **Hashing:** The filename is hashed into a 64-bit mask (a unique digital fingerprint).
2.  **Scan:** The driver scans the Anchor slots in the Cortex.
3.  **Filter Check:** It compares the fingerprint against the Anchor's tag.
    *   Mismatch: Skipped instantly (Cost: ~3 CPU cycles).
    *   Match: The driver performs a full string comparison to confirm it's the right file.
4.  **Load:** Upon a match, the Anchor is loaded, and the ballistic variables ($G, V$) are ready for I/O.

---

## 6. Storage Profiles

The physical layout of the volume adapts to the hardware it lives on. HN4 isn't "one size fits all."

| Profile | Target Hardware | Block Size | Optimization Strategy |
| :--- | :--- | :--- | :--- |
| **GENERIC** | NVMe / SATA SSD | 4KB | Standard Ballistic Allocation. Balanced for speed and space. |
| **PICO** | Microcontrollers | 512B | **Zero-RAM Mode.** Disables RAM bitmaps. It reads flash directly to check for free space. Runs on <2KB RAM. |
| **SYSTEM** | Bootloaders | 4KB | **The Launchpad.** Moves metadata to the center of the disk to minimize seek times. Reserves the first 1GB for contiguous boot files. |
| **GAMING** | Workstations | 64KB | **Outer Rim Bias.** On hard drives, the outer edge spins faster. This profile forces game assets to the outer rim for speed. |
| **AI** | H100 / Training Clusters | 64KB - 2MB | **Tensor Tunneling.** Uses massive blocks to match GPU memory pages. Enables the path-aware striping mentioned above. |
| **ARCHIVE** | Tape / SMR HDD | 64KB+ | **Linear Append.** Enforces strictly sequential writes. Ideal for Shingled Magnetic Recording (SMR) or Tape drives that hate random writes. |

---

## 7. Developer Integration

To integrate HN4 into a kernel or firmware project, you must meet these requirements:

1.  **Compiler:** C99 or C11 compliant compiler (GCC, Clang, MSVC).
2.  **Endianness:** The driver handles endianness internally, but `hn4_endians.h` must be configured if the target is Big Endian.
3.  **Hardware Abstraction Layer (HAL):** You must implement `hn4_hal.c` to map the driver to your specific hardware. Hooks required:
    *   `hn4_hal_mem_alloc` / `free`: Map to `kmalloc` or your system heap.
    *   `hn4_hal_submit_io`: Map to your block device driver (NVMe queue, AHCI, or SPI).
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
Once linked, the API usage is simple:

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
