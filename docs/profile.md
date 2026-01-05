# HN4 PROFILE ARCHITECTURE
### *Adaptive Physics for Specialized Workloads*
**Specification:** v6.0 | **Mechanism:** `Superblock.format_profile`

---

## 1. Abstract

A "One Size Fits All" filesystem is a myth.
*   **Databases** need atomic 4KB random writes.
*   **Video Archives** need 64MB sequential streams.
*   **AI Training** needs zero-copy tensor mapping.
*   **Games** need low-latency texture fetching.
*   **Operating Systems** need instant boot speeds and immutable binaries.

Legacy systems (ext4, NTFS) try to compromise using tuning flags.
**HN4 uses Profiles.**

A Profile is not just a config setting; it is a **Compiler Directive for the Driver Logic**. When a volume is mounted, the Profile dictates which mathematical constants ($V, M$) are used, how the Allocator behaves, and which hardware queues are prioritized.

---

## 2. THE SYSTEM PROFILE (`HN4_PROFILE_SYSTEM`)
**Target:** OS Boot Drives (Root), Live ISOs, Container Images.
**Goal:** Minimize Boot Time and harden security against Malware.

### 2.1 The "Launchpad" Geometry
Standard filesystems scatter metadata randomly or place it at the start, forcing the drive head (or NAND controller) to jump between Metadata and Data during boot.

**The HN4 Mechanism (Implemented in `hn4_format.c`):**
1.  **D0 Cortex (Metadata) at 50%:** The Anchor Table and Bitmap are moved to the physical **middle** of the drive.
2.  **D1 Flux (Data) at 0%:** The Data Region starts immediately after the Superblock/Epoch Ring (LBA ~260).
3.  **Result:** The physical start of the disk (LBA 0 to LBA 262,144) is purely **Binary Payload**.

### 2.2 The Hot Zone (LBA 0-1GB)
The Allocator enforces a strict segregation policy:
*   **Boot Files (Kernel/Drivers):** Tagged with `HINT_BOOT`. Forced into **LBA 0 - 1GB**.
*   **User Files:** Forced to start **after** 1GB.
*   **Behavior:** The Bootloader does not need to "seek" for files. It can issue a single **Linear Read** of the first 1GB into RAM. This is the theoretical maximum speed of the hardware.

### 2.3 Immutable Core
Files tagged `HINT_BOOT` automatically receive the `PERM_IMMUTABLE` flag unless explicitly overridden. This prevents malware from tampering with system binaries at the block level, even with Root privileges, unless the system is rebooted into Maintenance Mode.

---

## 3. THE AI PROFILE (`HN4_PROFILE_AI`)
**Target:** Machine Learning Training Clusters, NPU Interconnects.
**Goal:** Maximize Throughput ($>14$ GB/s) and bypass CPU overhead.

### 3.1 The "Tensor Tunnel" (Zero-Copy)
In traditional IO, data is copied: `Disk -> Kernel RAM -> User RAM -> GPU VRAM`. This burns CPU cycles and triples memory bandwidth usage.

**The HN4 Mechanism:**
1.  **Block Size ($M=14$):** The Allocator forces the Fractal Scale to **64 MB** unless small files are detected.
    *   *Why:* This aligns with the Page Size of modern GPU MMUs.
2.  **Pinned Trajectories:** The Allocator refuses to fragment AI files. It searches for contiguous regions ($V=1$) even if it takes longer to allocate.
3.  **The Tunnel:**
    *   When an application requests a file, the driver does not read it.
    *   It calculates the physical addresses (LBA list).
    *   It constructs a **PCIe Peer-to-Peer (P2P)** transaction list.
    *   It commands the NVMe Controller to DMA the data **directly** to the GPU's BAR address.
    *   *Result:* The CPU sees `0%` Load during a 20TB training run.

### 3.2 The Cortex Cleanup
*   **Logic:** AI datasets contain millions of small metadata files (labels, configs).
*   **Optimization:** The Driver reserves a massive **256MB Nano-Cortex** in RAM. It pre-loads all Anchors with the `CLASS_MATRIX` tag.
*   **Effect:** Searching for a specific training sample becomes a RAM-speed hash lookup, eliminating the "Metadata Stall" common in epoch training.

---

## 4. THE GAMING PROFILE (`HN4_PROFILE_GAMING`)
**Target:** Steam Deck, PS5-class Consoles, High-Performance Desktops.
**Goal:** Eliminate "Stutter" (Latency Spikes) and Texture Pop-in.

### 4.1 The Ludic Protocol (Texture Ballistics)
Modern games stream textures based on the camera angle. If the drive takes >16ms to fetch a texture, the frame drops.

**The Solution: Orbital LoD Mapping**
HN4 maps the **Trajectory Shells ($k$)** to **Level of Detail (LoD)**.

*   **$k=0$ (Primary Orbit):** Contains the Low-Res Mesh and 1080p Textures.
    *   *Placement:* Allocated on the fastest NAND channels (SLC Cache).
*   **$k=1$ (Secondary Orbit):** Contains 4K Textures.
*   **$k=2$ (Tertiary Orbit):** Contains 8K / Ray-Tracing Data.

**The Prefetch Logic:**
When the Game Engine requests the Mesh ($k=0$):
1.  The Driver serves $k=0$ immediately (Priority Queue).
2.  **Simultaneously**, it issues speculative reads for $k=1$ and $k=2$.
3.  Because the location of $k=1$ is mathematically deterministic ($LBA_{k1} = LBA_{k0} + \text{Offset}$), the driver doesn't need to look up metadata.
4.  *Result:* By the time the GPU renders the frame, the high-res textures are already in the buffer.

### 4.2 Isochronous Audio Lanes
*   **Problem:** Heavy texture loading saturates the bus, causing audio crackle.
*   **HN4 Fix:** The driver reserves **NVMe Queue 7** exclusively for files tagged `TYPE_AUDIO` (if HAL supports multiple queues).

---

## 5. THE PICO PROFILE (`HN4_PROFILE_PICO`)
**Target:** Microcontrollers (ESP32, ARM Cortex-M), Floppy Disks, Embedded Sensors.
**Goal:** Run in < 1KB of RAM.

### 5.1 The "Mono-Orbit" Constraint
Calculating complex 64-bit quadratic trajectories is too heavy for an 8-bit or 32-bit MCU.

*   **Logic:** The Driver disables the "Ballistic Engine."
*   **Vector Lock:** $V=1$ (Sequential).
*   **Collision Lock:** $k=0$ (No Retries).
*   **Behavior:** The filesystem behaves like a **Raw Stream**. It writes data linearly. If it hits a used block, it just skips it (Linear Probing).

### 5.2 Windowed Allocation
Instead of loading a 4MB Bitmap into RAM (which the device doesn't have):
*   **Windowing:** The driver reads the Bitmap from disk in **512-Byte Chunks** (`HN4_IO_CHUNK_SIZE` adaptable).
*   It scans the chunk for a `0` bit.
*   If found, it allocates. If not, it drops the chunk and reads the next 512 bytes.
*   *Result:* Infinite capacity support with constant RAM usage.

---

## 6. THE ARCHIVE PROFILE (`HN4_PROFILE_ARCHIVE`)
**Target:** Tape Drives, HDD Arrays, Cold Storage.
**Goal:** Maximum Density and Bit-Rot Protection.

### 6.1 The "Inertial Damper" (Head Control)
Hard Drives hate seeking (10ms latency).
*   **Logic:** The Driver disables the "Shotgun Read" (Parallel requests).
*   **Force Sequential:** It behaves like a Tape Drive. Writes are buffered until **128 MB** (or Zone Size), then flushed as a single continuous stream.

### 6.2 Deep Compression (ZSTD)
*   **Behavior:** Every block is compressed with **ZSTD**.
*   **D2 Packing:** Unlike D1 (which pads to 4KB for alignment), D2 Stream Mode packs compressed blocks byte-tight.
    *   *Result:* 20-30% capacity gain over standard filesystems.

---

## Summary Matrix

| Feature | **SYSTEM** | **AI** | **GAMING** | **PICO** | **ARCHIVE** |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Primary Goal** | **Instant Boot** | **Bandwidth** | **Latency** | **Low RAM** | **Density** |
| **Fractal Scale ($M$)** | 0 (4KB Fixed) | 14 (64MB) | 4 (64KB) | 0 (4KB/512B) | Variable |
| **Trajectory ($V$)** | Linear ($V=1$) | Tensor Stride | Interleaved | Linear ($V=1$) | Linear ($V=1$) |
| **Allocation** | **Hot Zone (0-1GB)** | Contiguous | Scattered | Windowed | Stream |
| **RAM Usage** | Low | High | Medium | Tiny (<1KB) | Low |
| **Special Feature** | **XIP / Immutable** | GPU Direct | Texture Prefetch | XIP Support | Reed-Solomon |

This architecture allows the same **HN4 Specification** to run on a $2 microcontroller, a Boot Drive, and a $200,000 AI Cluster, behaving natively on each.