# HN4 Tensor Stream Layer Specification

**Module:** `hn4_tensor.c`
**Status:** Hardened / Production
**Version:** 2.1
**Role:** Read-Only Virtualization & Stream Aggregation

## 1. Architectural Overview

The Tensor Stream Layer provides a linearization abstraction over the HN4 Cortex. It projects a set of distributed, non-contiguous storage objects (Shards) into a single, byte-addressable virtual address space.

This layer is specifically designed for Large Model (LLM) weights and Vector Embeddings, where data exceeds the capacity of a single physical anchor or requires distributed allocation across non-uniform geometry.

### 1.1. Virtualization Scope
*   **Input:** A string-based Model Tag (e.g., `model:llama3-70b`).
*   **Mechanism:** Bloom Filter resonance scanning and strictly monotonic ID sorting.
*   **Output:** A contiguous `0..N` byte stream via `memcpy`-compatible semantics.

### 1.2. Physical Locality Disclaimer
While the layer presents a contiguous virtual address space, **physical locality is not guaranteed**. Shards may reside in different physical zones, erasure coding groups, or disparate regions of the media. The reader acts as a scatter-gather engine.

---

## 2. Internal State Structures

The runtime state is encapsulated in the opaque handle `hn4_tensor_ctx_t`. This structure functions as the Translation Lookaside Buffer (TLB) for the virtual stream.

```c
typedef struct {
    hn4_volume_t* vol;              // Reference to the physical volume
    hn4_anchor_t* shards;           // Array of Anchor metadata [0..N-1]
    uint64_t*     shard_offsets;    // Prefix Sum Array describing virtual boundaries
    uint32_t      shard_count;      // N
    uint64_t      total_size_bytes; // Sum of mass of all shards
    uint32_t      block_size;       // Physical Block Size (e.g., 4096)
    uint32_t      payload_cap;      // Usable data per block (BS - Header)
} hn4_tensor_ctx_t;
```

### 2.1. Payload Capacity vs. Block Size
A critical distinction in HN4 geometry is the relationship between physical block size and payload capacity. The Tensor layer operates on **Payload Units**, not physical blocks.

$$ \text{PayloadCap} = \text{VolBlockSize} - \text{sizeof(hn4\_block\_header\_t)} $$

Atomic read operations return exactly `PayloadCap` bytes of data. The header is stripped by the lower-level `hn4_read_block_atomic` routine before the data reaches the Tensor layer.

---

## 3. Initialization Sequence (`hn4_tensor_open`)

Initialization constructs the virtual memory map through a three-stage pipeline: Discovery, Ordering, and Geometry Mapping.

### 3.1. Stage 1: Resonance Scan (Discovery)
The engine invokes `hn4_ns_gather_tensor_shards` to scan the Cortex (D0). This utilizes a Bloom Filter for rapid rejection of non-matching anchors.

*   **Bloom Logic:** The input string `model_tag` is hashed via FNV-1a modified to generate three 6-bit indices. These indices set 3 bits in a 64-bit mask.
*   **Filter Check:** An anchor is a candidate if `(anchor.tag_filter & mask) == mask`.
*   **Collision Resolution:** Because Bloom Filters allow false positives, the Namespace layer performs a strict string comparison on the `target_name` (if stored in the inline buffer or extended metadata) to disambiguate collisions before returning the shard to the Tensor layer.

### 3.2. Stage 2: Monotonic Ordering
The gathered shards are sorted using `qsort` with the `_shard_cmp` comparator.

*   **Sort Key:** 128-bit `seed_id` (comprising `.hi` and `.lo` 64-bit integers).
*   **The Monotonic Contract:** The Tensor layer **assumes** that the writer created shards in sequential order and assigned monotonically increasing Seed IDs. The Reader **does not validate** that the data content actually flows logically from Shard A to Shard B; it strictly enforces ID-based ordering.
*   **Implication:** If shards are re-keyed or written out-of-order without ID management, the virtual stream will be scrambled.

### 3.3. Stage 3: Geometry Mapping (Prefix Sums)
The context allocates a `shard_offsets` array of size $N+1$. The engine iterates through the sorted shards to build the virtual map.

*   `shard_offsets[i]` = Virtual Start Address of Shard $i$.
*   `shard_offsets[i+1]` = Virtual End Address of Shard $i$.
*   `mass` = Logical file size in bytes.

**Constraints:**
1.  **Mass > 0:** Any shard reporting 0 mass triggers `HN4_ERR_DATA_ROT`. Zero-length shards break binary search invariants.
2.  **64-bit Limit:** The accumulator is a `uint64_t`. The protocol supports a maximum tensor size of $2^{64}-1$ bytes (approx 18.44 Exabytes). Overflow is not explicitly checked; it is assumed geometry limits are enforced at write time.

---

## 4. The Read Pipeline (`hn4_tensor_read`)

This function implements the Virtual-to-Physical (V2P) translation logic.

### 4.1. Bounce Buffering Strategy
The Tensor layer allocates a single transient **Bounce Buffer** for the duration of the read call.

*   **Size:** Strictly `ctx->payload_cap`.
*   **Purpose:** `hn4_read_block_atomic` performs a whole-block read and strips headers. It cannot write directly to a user buffer if the user request is misaligned or smaller than a block. The Bounce Buffer accepts the raw payload, and `memcpy` handles the transfer to the user buffer.

### 4.2. Shard Resolution (Binary Search)
The engine locates the target shard for the current `global_offset` using binary search on `shard_offsets`.
*   **Complexity:** $O(\log N)$ where $N$ is the shard count.
*   **Range:** `shard_offsets[i] <= global_offset < shard_offsets[i+1]`

### 4.3. Block Math
Once the shard and local offset ($O_{local}$) are identified, physical coordinates are derived:

$$ O_{local} = \text{GlobalOffset} - \text{shard\_offsets}[i] $$
$$ \text{BlockIndex} = \lfloor O_{local} / \text{PayloadCap} \rfloor $$
$$ \text{ByteInBlock} = O_{local} \pmod{\text{PayloadCap}} $$

**Geometry Error Check:** If `BlockIndex * PayloadCap` exceeds the shard's mass, `HN4_ERR_GEOMETRY` is returned. This defends against metadata corruption where `mass` does not match the physical block allocation.

### 4.4. Stream Loop
The pipeline iterates until the requested length is satisfied or EOF is reached:
1.  **Atomic Read:** Data is fetched into the Bounce Buffer via `hn4_read_block_atomic`. This handles CRC checks, decryption, and decompression.
2.  **Transfer:** Valid bytes are copied to `user_buffer + current_cursor`.
3.  **Advance:** The loop advances across block boundaries. If the end of a shard is reached, `shard_idx` increments, and `LocalOffset` resets to 0.

---

## 5. Semantic Behaviors & Constraints

### 5.1. End-of-File (EOF) Semantics
The reader enforces strict clamping. It does not return an error for reading past EOF; it performs a **Short Read**.
*   If `global_offset + len > total_size_bytes`, then `len` is clamped to `total_size_bytes - global_offset`.
*   If `global_offset >= total_size_bytes`, the function returns `HN4_ERR_INVALID_ARGUMENT` (Seek Error), effectively acting as EOF.

### 5.2. Error Propagation and Atomicity
The read operation is **synchronous and blocking**.
*   **All-or-Nothing:** If a read fails mid-stream (e.g., `HN4_ERR_DATA_ROT` on the 50th block), the function returns the error code immediately. It does **not** return a partial byte count. The state of the user buffer is undefined for the unwritten portion.
*   **Error Priority:**
    1.  `HN4_ERR_INVALID_ARGUMENT` (Bad params / Seek past EOF).
    2.  `HN4_ERR_NOMEM` (Bounce buffer alloc fail).
    3.  `HN4_ERR_GEOMETRY` (Metadata inconsistency).
    4.  `HN4_ERR_DATA_ROT` / `HN4_ERR_HW_IO` (Physical layer failures).

### 5.3. Concurrency and Reentrancy
*   **Context Safety:** The `hn4_tensor_ctx_t` structure is **read-only** after initialization. It can be shared across threads *if and only if* the underlying `hn4_volume_t` handle and HAL implementation are thread-safe.
*   **Read Safety:** `hn4_tensor_read` is reentrant. It allocates its own stack/heap state (Bounce Buffer) per invocation. Multiple threads can read from the same `ctx` simultaneously.

### 5.4. Bloom Filter Implications
While the Bloom Filter provides $O(1)$ rejection of irrelevant anchors, it introduces a dependency on the Namespace layer for correctness.
*   **False Positives:** Handled by string verification in `hn4_namespace.c`.
*   **Tag Collisions:** If two distinct models hash to the exact same 3 bit positions *and* have the exact same string tag (impossible by definition), they would be merged into the same stream. It is the user's responsibility to ensure Model Tags are unique.

---

## 6. Usage Recommendations

1.  **Alignment:** For maximum throughput, align read requests to `PayloadCap` boundaries. This minimizes `memcpy` overhead and ensures the Bounce Buffer is utilized efficiently (1 copy per block read).
2.  **Memory:** The `open` call performs `malloc` proportional to the number of shards ($N \times \text{sizeof}(\text{Anchor})$). For massive models with thousands of shards, ensure sufficient heap space is available.
3.  **Tearing:** The Tensor layer does not provide snapshot isolation across shards if the volume is mounted R/W. If the underlying anchors are modified during a read operation, the stream data may tear. Ensure the volume is quiescent or mounted Read-Only during Tensor streaming.