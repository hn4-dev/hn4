<div align="center">

# HN4
### **The Post-POSIX Filesystem.** 

<!-- Badges -->
[![Status](https://img.shields.io/badge/Status-Final-orange?style=for-the-badge)](https://github.com/)
![Platform](https://img.shields.io/badge/platform-Bare%20Metal%20%7C%20Embedded%20%7C%20Kernel-orange.svg?style=for-the-badge)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue?style=for-the-badge)](https://opensource.org/licenses/Apache-2.0)

<br />

</div>

**HN4** is a high-speed storage engine built for the physics of modern infrastructure. It optimizes for the specific constraints of the hardware—enforcing strict sequentiality for **Spinning Hard Drives** and **Zoned Storage (ZNS)**, while unleashing ballistic parallelism for **NVMe SSDs**.

If standard file systems are like filing cabinets, HN4 is a teleportation device.

---

## 1. System Overview

HN4 is a standalone driver. It doesn't need an operating system's standard libraries (like `libc`) to work. It uses **Ballistic Addressing** instead of lookup tables to manage storage.

This means the driver doesn't "search" for empty space. It calculates where data goes using math. Reading a file takes the exact same amount of time whether the drive is empty or 99% full.

### Key Features
*   **Constant Speed:** Performance never drops as the drive fills up.
*   **Flexible Size:** Works on tiny 1MB IoT chips and massive 18 Exabyte servers.
*   **Crash Safety:** Uses a transaction ring (Epochs) to ensure data is never corrupted by power loss, without the slowness of a journal.

---

## 2. How It Works (The Logic)

HN4 replaces complex file system structures with simple physics equations.

### 2.1. Ballistic Allocation (Finding Space)

**The Old Way:** Traditional file systems use a "Bitmap"—a giant map of every block on the disk—to find free space. When writing, the CPU has to scan this map. As the drive fills up, this scan gets slower.

**The HN4 Way:** HN4 **calculates** where data goes. It uses a trajectory formula.

*   **The Variables:**
    1.  **Gravity Center ($G$):** The starting point of the file on the disk.
    2.  **Velocity ($V$):** The jump size between blocks.
    3.  **Scale ($M$):** The size of the chunks being written.

*   **The Concept:**
    Imagine a clock face.
    *   **$G$** is where you start (e.g., 12 o'clock).
    *   **$V$** is how many hours you jump for every chunk of the file.
    
    If $V=1$, you write to 12, 1, 2, 3 (**Sequential**). This is used for HDDs and ZNS to prevent head thrashing.
    If $V=5$, you write to 12, 5, 10, 3 (**Scattered**). This is used for SSDs to maximize parallelism.
    
    Because this is pure math, the CPU knows exactly where "Chunk 500" is instantly, without looking it up in a list.

### 2.2. Atomic Writes (Safety)
HN4 never overwrites data in place. This prevents corruption if the power fails during a write.

1.  **Write New:** Data is written to a new, calculated location (Shadow LBA).
2.  **Flush:** The hardware is told to burn the data to physical storage.
3.  **Switch:** Only *after* the data is safe, the file's metadata is updated in RAM to point to the new location.
4.  **Free Old:** The old data block is marked as free.

---

## 3. AI & High Performance

HN4 has specific features for AI workloads (like training Large Language Models) where loading huge files quickly is critical.

### 3.1. Virtual Streams
Large AI models are often split into pieces across many drives. HN4's **Tensor Layer** scans the metadata region using a probability filter (Bloom Filter). It finds all parts of a model (e.g., `model:gpt-4`) instantly and presents them as one continuous file to the application.

### 3.2. Hardware Awareness
In servers with multiple GPUs, data travel time matters. HN4 checks which GPU is asking for data. It then forces the storage controller to save that data on the physical flash chips closest to that specific GPU. This minimizes latency across the motherboard.

---

## 4. Hyper-Cloud & Spatial Arrays

HN4 includes a native **Spatial Array Router** designed for Enterprise and Cloud environments. This allows HN4 to manage multiple physical disks directly, bypassing legacy RAID controllers.

### 4.1. Gravity Well Entanglement (Mirroring)
In this mode, HN4 writes data to multiple drives simultaneously for redundancy.
*   **Read:** Data is read from the drive with the lowest current load or latency.
*   **Zombie Defense:** If a write fails on one mirror, that drive is instantly marked offline, preventing "stale reads" without pausing the system.

### 4.2. Ballistic Sharding (Striping)
HN4 distributes data across drives mathematically based on the File ID.
*   **Deterministic Location:** The driver knows exactly which physical drive holds a specific file without checking a central index.
*   **Alignment:** Enforces 1MB alignment to ensure writes mesh perfectly with cloud storage stripe sizes.

### 4.3. Battery-Backed Optimization
When running in the **HYPER_CLOUD** profile, HN4 assumes the presence of battery-backed RAID controllers. It optimizes write throughput by disabling per-block flush commands (FUA), relying instead on Async Consistency for maximum IOPS.

---

## 5. Reading and Writing

HN4 treats Data (Payload) and Metadata (Maps) differently to keep speed high.

### 5.1. Writing Data
1.  **Calculate:** To change a file, the driver increments a version counter. This changes the math variables ($G, V$), creating a new path on the disk.
2.  **Write:** Data is sent to this new path.
3.  **Confirm:** The driver waits for the hardware to confirm the write.
4.  **Update:** The file's metadata in memory is updated to the new version.

### 5.2. Reading Data
Reading doesn't involve looking up pointers in a table. It involves re-calculating the location.

1.  **Project:** The driver reads the file's metadata to get the math variables ($G, V$).
2.  **Calculate:** It computes the target address.
3.  **Read & Check:** It reads the block and verifies:
    *   **Integrity:** Is the CRC correct?
    *   **ID:** Does this block belong to this file?
    *   **Version:** Is this the latest version?
4.  **Retry:** If verification fails (e.g., we found an old version), the driver calculates the next possible location and tries again.

### 5.3. Saving State
Data is saved immediately. Structure maps are saved during **Sync** or **Unmount**. The metadata is written to 4 physical locations (North, East, West, South) on the disk to ensure the drive remains readable even if parts of it degrade.

---

## 6. Finding Files (Namespace)

HN4 uses a flat list for metadata called the **Cortex**. There are no folders.

To find a file:
1.  **Hash:** The filename is turned into a unique digital number.
2.  **Scan:** The driver scans the Cortex for this number.
3.  **Load:** If found, the file's parameters ($G, V$) are loaded and ready for I/O.

---

## 7. Storage Profiles

HN4 changes its layout based on the device it is formatting.

| Profile | Hardware | Block Size | Optimization |
| :--- | :--- | :--- | :--- |
| **GENERIC** | NVMe / SATA SSD | 4KB | Standard balanced mode. |
| **PICO** | Microcontrollers | 512B | **Low RAM Mode.** Disables memory maps. Reads the disk directly to find space. Runs on <2KB RAM. |
| **SYSTEM** | Bootloaders | 4KB | Moves metadata to the middle of the drive for faster access. Reserves space for boot files. |
| **GAMING** | PC Workstations | 64KB | Forces game assets to the outer edge of hard drives (where read speeds are faster). |
| **AI** | GPU Clusters | 64KB - 2MB | Uses massive blocks to match GPU memory. Enables location-aware storage. |
| **ARCHIVE** | Tape / HDD | 64KB+ | **Linear Mode.** Writes data in a strict straight line ($V=1$). Essential for Tape drives or Shingled HDDs that act poorly with random writes. |
| **HYPER_CLOUD** | Enterprise Arrays | 64KB | **Native Arrays.** Enforces 1MB alignment for striping. Activates Spatial Router. Disables speculative compression to maximize CPU IOPS. |

---

## 8. Developer Integration

To use HN4 in your OS or firmware:

1.  **Compiler:** Use any standard C99/C11 compiler (GCC, Clang, MSVC).
2.  **Endianness:** The driver handles byte-order automatically.
3.  **HAL (Hardware Layer):** You must provide a `hn4_hal.c` file that connects the driver to your hardware. You need to implement:
    *   `mem_alloc` / `free`: Connect to your system's heap.
    *   `submit_io`: Connect to your disk driver (read/write blocks).
    *   `get_time`: A simple clock function.

---

## 9. Build & Usage

### Building for Test (Linux/macOS)
```bash
gcc -std=c11 -O2 \
    src/*.c \
    test/sim_hal.c \
    test/main_test.c \
    -I include/ \
    -D_GNU_SOURCE \
    -o hn4_test

./hn4_test
```

### Building for Kernel / Bare Metal
Compile the core driver without the test harness.

```bash
gcc -std=c11 -O3 -ffreestanding -nostdlib \
    -c src/hn4_core.c \
    -c src/hn4_addr.c \
    -c src/hn4_allocator.c \
    -c src/hn4_read.c \
    -c src/hn4_write.c \
    -c src/hn4_tensor.c \
    -c src/hn4_io_router.c \
    -I include/ \
    -o hn4_driver.o
```

### Code Example: Mounting a Volume
```c
#include "hn4.h"

void mount_example(hn4_hal_device_t* phys_dev) {
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t params = {0};

    // 1. Initialize HAL
    hn4_hal_init();

    // 2. Mount Volume (Auto-detects settings)
    hn4_result_t res = hn4_mount(phys_dev, &params, &vol);

    if (res == HN4_OK) {
        // 3. Find a File
        hn4_anchor_t file_anchor;
        res = hn4_ns_resolve(vol, "config/boot.cfg", &file_anchor);
        
        if (res == HN4_OK) {
            // 4. Read Data
            char buffer[4096];
            // Read Block 0
            hn4_read_block_atomic(vol, &file_anchor, 0, buffer, 4096);
        }
        
        // 5. Unmount (Save state)
        hn4_unmount(vol);
    }
}
```