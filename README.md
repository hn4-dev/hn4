<div align="center">

# HN4
### The Next-Generation Constant-Time Storage Engine

<!-- Badges -->
[![Status](https://img.shields.io/badge/Status-Final_Testing-orange?style=for-the-badge)](https://github.com/)
[![Release](https://img.shields.io/badge/Release-~8_Days-success?style=for-the-badge)](https://github.com/)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue?style=for-the-badge)](https://opensource.org/licenses/Apache-2.0)

<br />

</div>

---


# HYDRA-NEXUS 4 (HN4)

> **Do you want something that boots on a 486, hums on Ryzen, runs in a phone, a router, a satellite, a toaster, and a mysterious future RISC-V cube someone prints in their garage? You came to the right place.**

---

> **🚀 Launch Countdown:** The first stable release candidate drops in **8 days**.

### 🚧 Project Status & Verification
**HN4 is currently in active development.** The core architecture is solid and the drivers pass validation, but the API is still evolving. We recommend using this release for testing, benchmarking, and research. Full production certification is in progress.

**Don't just take our word for it.** You can audit the engine's stability right now. Simply run `make` (Linux/ARM) or `run.bat` (Windows) and watch **thousands of architectural, fuzzing, and unit tests** pass before your eyes to understand the current progress.

---

## 💾 What is HN4?

HN4 is a freestanding file system driver designed from first principles for high-performance and constrained environments. It abandons the traditional B-Tree/Inode allocation models used by EXT4 and XFS in favor of **Ballistic Allocation**.

By calculating data location via deterministic math rather than looking it up in a tree or scanning a bitmap, HN4 achieves **amortized $O(1)$** allocation and lookup times, regardless of disk capacity or fragmentation levels.

It bridges the gap between embedded storage and high-performance computing. Whether you are logging sensor data on a satellite with 4KB of RAM or training LLMs on a 100TB ZNS NVMe array, HN4 provides a unified, deterministic, and crash-safe storage fabric.

*   **No Trees:** Eliminates the $O(\log n)$ traversal penalty.
*   **No Aging:** Write performance remains constant at 1% capacity and 99% capacity.
*   **No Dependencies:** Designed to link directly into kernels, bootloaders, or firmware without `libc`, `malloc`, or POSIX threads.

---

## 📖 The Dictionary: Lore vs. Engineering

HN4 uses specific nomenclature to describe its internal mechanisms. Here is the translation layer for system engineers:

### 1. The Void Engine (Ballistic Allocation)
**Concept:** Instead of scanning a bitmap for free space, the system calculates a target LBA based on the file's ID and sequence number.
**Mechanism:** **Deterministic Hashing.**
$$LBA = (Gravity + Sequence \times Vector) \pmod{Capacity}$$
If the target block is occupied, it does not scan linearly. It applies a **"Gravity Assist"**—a bitwise rotation and XOR shift—to mathematically select a new, uncorrelated candidate block. This bounds tail latency even at 99% capacity.

### 2. Shadow Hop (Atomic Persistence)
**Concept:** Files are never overwritten in place. Data "hops" to a new trajectory during updates.
**Mechanism:** **Copy-on-Write (COW).**
New data is written to a fresh block. Only after the write is verified (CRC32C) is the metadata (Anchor) updated to point to the new location. This guarantees crash consistency without the overhead of a journal.

### 3. The Helix (Auto-Medic)
**Concept:** An immune system that detects and repairs corruption on the fly.
**Mechanism:** **Inline Scrubbing & SEC-DED.**
Every allocation bitmap word is protected by Hamming Code (Single Error Correction, Double Error Detection). If a bit flips in RAM or on disk, the driver detects it, corrects the value in memory, and transparently flushes the fix to disk.

### 4. Wormhole Mounting
**Concept:** Mounting a volume with specific overlay properties.
**Mechanism:** **Virtualization & Identity Aliasing.**
Allows a volume to be mounted with a virtual capacity or specific permission masks (e.g., forcing a read-only snapshot to behave as a writable volume via a RAM overlay).

---

## 🛡️ Failure Model

HN4 does not require an external `fsck` utility. The driver contains an embedded "immune system" that runs continuously.

| Threat | Mechanism | Status |
| :--- | :--- | :--- |
| **Sudden Power Loss** | **Epoch Ring.** A circular buffer of atomic timestamps ensures the file system rolls back to the last consistent transaction (max 5s data loss). No partial metadata states ever exist. | ✅ Implemented |
| **Dirty Mounts** | **Convergent Healing.** If a volume was unplugged without unmounting, HN4 performs a **Non-Destructive State Convergence**. It scans the Cortex (metadata) and mathematically reconstructs the allocation bitmap in RAM. Unlike `fsck`, it does not scan data blocks, allowing it to mount "Dirty" Petabyte volumes in seconds. | ✅ Implemented |
| **Bit Rot (Runtime)** | **The Auto-Medic.** Verification happens on *every* read. If a block fails CRC32C validation, the driver triggers **Inline Repair**. It retrieves valid data (via hysteresis or parity), overwrites the corrupted physical sector, and returns success to the application without interruption. | ✅ Implemented |
| **Torn Writes** | **Shadow Paging.** Superblocks are never overwritten directly; valid state toggles between 4 replicas based on Generation ID. | ✅ Implemented |
| **Bad Sectors** | **Toxic Quarantine.** If a physical block permanently fails IO, it is marked "Toxic" in the bitmap. The Void Engine then treats that LBA as a mathematical singularity, routing all future allocations around it. | ✅ Implemented |

---

## 🛠️ Profiles & Capabilities

HN4 adapts its physical layout based on the target `format_profile`:

### 🔹 PICO (Embedded / IoT)
*   **Target:** Microcontrollers (ESP32, ARM Cortex-M).
*   **Block Size:** 512 Bytes.
*   **Mechanism:** **Zero-RAM Bitmap.** It disables the in-memory allocation map entirely. It uses deterministic mapping and direct flash reads to avoid `malloc` completely.
*   **Memory Cost:** < 2KB stack usage.

### 🔹 SYSTEM (Boot / OS Root)
*   **Target:** Operating System Roots, Firmware, Hypervisors.
*   **Layout:** **"The Launchpad."** Unlike standard file systems that scatter metadata, this profile moves the Cortex (Metadata) and Bitmap to the physical *middle* of the disk.
*   **Hot Zone:** The first 1GB of the disk is reserved exclusively for `HN4_HINT_BOOT` allocations.
*   **Benefit:** Enables linear, sequential reads for the bootloader (creating a contiguous stream) and supports XIP (Execute-In-Place) directly from storage.

### 🔹 GAMING (Ludic Mode)
*   **Target:** High-Performance Workstations, Game Consoles.
*   **Mechanism:** **Outer Rim Allocation.** The allocator prioritizes the physical outer edge of the platter (on HDDs) or the SLC Cache tier (on SSDs) for assets marked `HN4_TYPE_LUDIC`.
*   **Constraint:** Enforces a 16TB volume limit to guarantee seek latency remains within frame-time budgets (<16ms).

### 🔹 AI (Tensor Store)
*   **Target:** Training Clusters, H100/TPU Pods.
*   **Mechanism:** **Tensor Tunneling.** Enables `HN4_HW_GPU_DIRECT` flags.
*   **Alignment:** Enforces strict 2MB alignment on data blocks to match GPU memory page sizes, allowing Peer-to-Peer DMA (Storage $\to$ GPU) that bypasses the CPU entirely.
*   **Scalability:** Allocates an expanded Cortex region to handle hundreds of millions of tiny training samples without metadata bottlenecks.

### 🔹 GENERIC (Desktop / Server)
*   **Target:** NVMe SSDs, SATA HDDs.
*   **Block Size:** 4KB.
*   **Mechanism:** Standard Ballistic allocation with full bitmap caching.

### 🔹 ARCHIVE (Cold Storage)
*   **Target:** Tape, SMR HDDs.
*   **Block Size:** 64KB - 256MB.
*   **Status:** *Experimental.* Compression headers and Reed-Solomon parity hooks are defined in the spec but currently stubbed in the driver.

---

## 🚫 What HN4 is NOT

To save you time, here is what this file system is not:

1.  **It is not POSIX compliant.** It does not store user IDs, groups, or symlinks in the standard way. It uses Sovereign Keys and Tethers for identity.
    *   *Note: A FUSE-based compatibility shim (`hn4-posix`) is currently in development to allow mounting on Linux systems with standard `chmod`/`chown` emulation.*
2.  **It is not a "Log-Structured File System" (LFS).** While it uses log-structured commit semantics (Epochs) for consistency, it does not use a Segment Cleaner or garbage collection.
3.  **It is not finished.** While the core logic passes bare-metal architectural tests (including 32-bit overflow protection and alignment checks), it has not yet undergone third-party security auditing.

## 🔨 Integration

HN4 is a header-only style library integration.

1.  **Implement the HAL:** Provide functions for `hn4_hal_submit_io` and `hn4_hal_mem_alloc` in `hn4_hal.c`.
2.  **Compile:**
    ```bash
    gcc -std=c99 -c hn4_core.c -o hn4.o
    ```
3.  **Mount:**
    ```c
    hn4_volume_t* vol;
    // The driver automatically handles endianness swapping and geometry checks
    if (hn4_mount(my_device, &vol) == HN4_OK) {
        // Ready
    }
    ```

## 📚 Glossary

*   **Anchor:** The metadata pointer describing a file (similar to an Inode).
*   **Cortex:** The region of the disk (D0) where Anchors are stored.
*   **Epoch Ring:** A circular buffer used to track atomic transaction history.
*   **Sovereign Keys & Tethers:** The HN4 identity and permission model, replacing POSIX UIDs.

---

*(c) 2025 The Hydra-Nexus Team.*
