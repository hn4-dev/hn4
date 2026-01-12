
# HN4 Tensor Stream Architecture
**Module:** `hn4_tensor.c`
**Status:** Hardened / Production
**Version:** 2.1

## 1. Problem Statement: The Memory Wall

High-throughput workloads like Large Language Model (LLM) training and inference are typically I/O bound. The standard filesystem stack introduces latency through repeated memory copies and context switching.

When an application requests bulk data from a standard filesystem:
1.  **Context Switch:** A syscall interrupts execution.
2.  **Kernel Buffer:** The OS reads data from storage into the kernel page cache.
3.  **User Copy:** The OS copies data from kernel space to user space RAM.
4.  **Driver Copy:** The GPU driver copies data from user space to a pinned DMA buffer.
5.  **PCIe Transfer:** Data is finally moved to VRAM.

This process consumes significant CPU cycles and saturates the system memory bus, leaving the GPU idle while waiting for data.

## 2. The Tensor Stream Solution

HN4 implements a dedicated **Tensor Stream Layer** to bypass the operating system's buffer cache and facilitate direct hardware-to-hardware transfers.

### 2.1 Virtualization & Aggregation
Large models are rarely stored as single contiguous files due to fragmentation risks. HN4 shards these datasets across multiple physical anchors but presents them as a single virtual address space.

*   **Discovery:** The driver scans the Cortex metadata region using a **Bloom Filter**. It identifies all Anchors associated with a specific Model Tag (e.g., `model:llama3-70b`) in $O(N)$ linear time.
*   **Ordering:** Shards are sorted by their 128-bit Seed IDs.
*   **Mapping:** A runtime translation layer maps logical offsets in the virtual stream to physical blocks on the media. This allows applications to `mmap` or stream multi-terabyte datasets without managing individual file shards.

### 2.2 Path-Aware Striping (Topology Awareness)
In multi-socket or multi-GPU systems, data locality impacts performance. Accessing storage attached to a remote NUMA node or PCIe switch incurs latency penalties.

HN4 queries the system topology at mount time. When allocating storage for tensor shards:
1.  **Affinity Check:** The allocator inspects the hardware context of the calling thread (e.g., GPU ID).
2.  **Locality Bias:** It prioritizes allocation in physical Zones or Namespaces that share a PCIe root complex with the requesting accelerator.
3.  **Result:** This minimizes traffic across QPI/UPI interconnects during high-bandwidth loading operations.

### 2.3 Direct Memory Access (DMA) Alignment
To enable efficient Peer-to-Peer (P2P) transfers between NVMe SSDs and GPUs (bypassing the CPU), memory alignment is strictly enforced.

*   **Constraint:** Files tagged with `CLASS_MATRIX` are aligned to **2MB boundaries**.
*   **Rationale:** This matches the Huge Page size used by GPU Translation Lookaside Buffers (TLBs).
*   **Mechanism:** The allocator pads the start of the file allocation to ensure the physical LBA aligns with the hardware requirements for direct DMA.

## 3. Metadata Optimization

AI training workloads often involve iterating over millions of small files (images, text snippets). This access pattern causes thrashing in traditional B-Tree based filesystems.

### 3.1 Nano-Cortex (RAM Caching)
When the `HN4_PROFILE_AI` profile is active, the driver allocates a specialized metadata cache called the **Nano-Cortex**.

*   **Pre-Load:** Upon mount, the driver scans the primary Cortex region and loads all Anchors tagged `CLASS_MATRIX` into a hash table in RAM.
*   **Lookups:** File open operations become $O(1)$ RAM lookups, eliminating disk seeks for metadata retrieval.
*   **Payload:** Only the payload data requires physical I/O.

### 3.2 Deterministic Shuffling
Training loops require data randomization to prevent overfitting. HN4 supports hardware-accelerated shuffling.

*   **Legacy Method:** Applications shuffle file lists in user space, incurring CPU overhead.
*   **HN4 Method:** The driver accepts a randomization seed. By applying a bitwise permutation to the iteration order of the Nano-Cortex, the driver streams data in a pseudo-random sequence directly from the physical media. This offloads the shuffle logic to the storage controller.

## 4. Performance Characteristics

The architectural changes in the Tensor Stream Layer target specific performance metrics:

1.  **Throughput:** By enabling zero-copy paths and ensuring 2MB alignment, the system can saturate the PCIe bus bandwidth during model loading.
2.  **Latency:** Removing the kernel page cache copy reduces the time-to-first-byte latency.
3.  **CPU Efficiency:** Offloading address calculation and shuffling logic reduces CPU usage, freeing cores for data preprocessing tasks.

## 5. Usage Constraints

*   **Memory Overhead:** The Nano-Cortex requires system RAM proportional to the number of files. Approximately 1GB of RAM is needed per 100 million files.
*   **Hardware Support:** Zero-copy P2P transfer requires compatible hardware (PCIe Root Complex with P2P support) and drivers (e.g., NVIDIA GPUDirect or AMD ROCm). On unsupported hardware, the driver falls back to standard optimized buffered reads.