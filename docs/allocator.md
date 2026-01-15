# THE VOID ENGINE: ARCHITECTURAL SPECIFICATION
**Component:** `hn4_allocator.c`
**Status:** Implementation Standard v16.1
**Paradigm:** Stateless Probabilistic Allocation

---

## 1. Abstract: The Elimination of State

Traditional allocators (Ext4, NTFS, malloc) are **Stateful**. They rely on complex data structures (Bitmaps, Free Lists, B-Trees) to track available space. Allocation requires traversing these structures, leading to $O(N)$ or $O(\log N)$ degradation as fragmentation increases.

**The Void Engine is Stateless.**

It does not search for free space; it calculates a **Trajectory** where data *should* exist based on a deterministic physical equation. It then queries the memory map to verify if that specific coordinate is available.

*   **Logic:** Instead of asking "Where is a hole?", HN4 asks "Is Coordinate $X$ available?"
*   **Result:** Allocation complexity is decoupled from Disk Capacity and Fragmentation. Allocating a block on a 1.44MB Floppy takes the same CPU cycles as allocating on a 100PB Array.

---

## 2. NVM & Hardware Mitigation

### 2.1 The NVM Fast Path
When the media is identified as **Byte-Addressable Non-Volatile Memory** (e.g., Intel Optane, MRAM, CXL), the Allocator engages a zero-overhead fast path.

*   **Flag:** `HN4_HW_NVM` (0x4000).
*   **Optimization:** Skips Software ECC calculation and 128-bit CAS loop.
*   **Mechanism:** Uses atomic 64-bit operations directly on the bitmap.
*   **Throughput:** ~2.5x increase on DAX-mapped volumes.

### 2.2 Volatile Cache Mitigation
Consumer SSDs often acknowledge writes before data is durable on NAND. To prevent bitmap desynchronization on power loss:

*   **Hazard:** "Phantom Collision" (OS marks block used; Drive loses data; Bitmap and Data desynchronize).
*   **Mitigation:** `memory_order_seq_cst` fences are enforced on bitmap updates (`_bitmap_op`). This prevents CPU reordering of the bitmap claim relative to the data write, ensuring strict allocation ordering.

---

## 3. The Mathematics: Addressing Equation

The physical location (LBA) of block $N$ is derived from four variables using the **Universal Trajectory Function**.

Based on `_calc_trajectory_lba` in `hn4_allocator.c`:

$$ LBA_{phys} = LBA_{flux} + \left( [ (N_{cluster} \pmod{\Phi}) \times (V \pmod{\Phi}) + \Theta(k) ] \pmod{\Phi} \right) \times S + (G_{offset}) $$

### 3.1 Variables
1.  **$G$ (Base Offset):** Random seed selected at file creation.
2.  **$N$ (Sequence):** Logical block index. (Clustered for locality).
3.  **$V$ (Stride):** The allocation step size.
    *   Must be **Coprime** to the Allocation Window ($\Phi$) to guarantee coverage.
    *   Defaults: `1` (HDD/ZNS), `Prime` (SSD).
4.  **$\Phi$ (Window):** The size of the allocation region in blocks.
5.  **$\Theta(k)$ (Collision Offset):** Orbital offset for retry attempts ($k=0..12$).

### 3.2 The Coprimality Invariant

To prevent "Harmonic Locking" (looping over a subset of blocks), the Allocator enforces:

$$ \gcd(V, \Phi) \equiv 1 $$

**Implementation Logic:**
1.  **Detection:** Calculate `_gcd(V, phi)`.
2.  **Correction:** If not 1, perturb $V$ (add 2) until coprimality is achieved.
3.  **Failsafe:** If no coprime found in 32 tries, force **V = 1** (Linear).

**Mathematical Guarantee:**
Because $\gcd(V, \Phi) = 1$, the trajectory $T(n) = (n \times V) \pmod \Phi$ generates a **Cyclic Group of Order $\Phi$**. The sequence visits every unique block exactly once before repeating, guaranteeing 100% address space coverage.

---

## 4. Algorithmic Workflow

### 4.1 Genesis (Allocation - Phase 1)
New file creation uses a **Monte Carlo Probe** (`hn4_alloc_genesis`).

1.  **Capacity Check:** If saturation $> 90\%$, abort to **Horizon** (Phase 3).
2.  **Probe:** Select random integer $G_{cand}$.
3.  **Simulation:** Calculate trajectory for Head Block ($N=0$) at $k=0$.
4.  **Verification:** Query the **L3 Allocation Bitmap** (RAM).
    *   *Query:* "Is bit at $LBA(G_{cand})$ currently 0?"
5.  **Decision:**
    *   **Success:** Claim via CAS. $G_{cand}$ becomes permanent `Anchor.gravity_center`.
    *   **Collision:** Retry with new random $G$.
    *   *Limit:* Hard cap at 20 retries (`HN4_MAX_PROBES`).

**O(1) Justification:** At 80% capacity, finding a free slot within 20 probes is a statistical certainty. The algorithm samples rather than scans.

### 4.2 Atomic Relocation (Write/Update)
HN4 utilizes **Copy-on-Write** semantics via trajectory shifting.

To update Block $N$:
1.  **Current:** $LBA_{old} = f(G, V, N, k_{current})$.
2.  **Next:** $LBA_{new} = f(G, V, N, k_{current} + 1)$.
3.  **Verify:** Check Bitmap for $LBA_{new}$.
    *   If occupied, increment $k$ and retry.
4.  **Commit:** Write data to $LBA_{new}$.
5.  **Release:** $LBA_{old}$ is logically discarded when Anchor generation updates.

### 4.3 Vector Mutation (Phase 2)
If the local neighborhood ($k=0..3$) is full:

*   The engine engages **Vector Mutation** (Gravity Assist).
*   **Math:** $V_{assist} = \text{ROTL}(V, 17) \oplus \text{0xA5A5A5...}$
*   **Effect:** A bit-shift applies a chaotic transformation to the vector, teleporting the trajectory to an uncorrelated region of the disk.
*   **Benefit:** Defeats the "Birthday Paradox" of hashing by decorrelating collision chains.

---

## 5. Memory Map: Hierarchical Bitmap

HN4 maintains a hierarchical bitmap in RAM for instant verification.

### 5.1 L3: The Allocation Map
*   **Granularity:** 1 bit = 1 Physical Block.
*   **Structure:** `hn4_armored_word_t` (64-bits Data + 8-bits ECC).
*   **Updates:** 128-bit CAS (Standard) or 64-bit Atomic (NVM).

### 5.2 L2: The Summary Map
*   **Granularity:** 1 bit = 512 Blocks (2MB).
*   **Logic:** If `L2_Bit[i] == 0`, **ALL** 512 blocks are free.
*   **Optimization:** Allocator checks L2 first.
    *   If `L2 == 0`: Skip L3 check. **64x Speedup.**
    *   If `L2 == 1`: Check specific bits in L3.

---

## 6. Saturation Logic: The Horizon (Phase 3)

When disk usage $>90\%$, probabilistic collisions degrade performance.

*   **Trigger:** `_check_saturation`.
*   **Action:** New allocations route to **D1.5 (Horizon)**.
*   **Mechanism:**
    *   A simple **Linear Ring Buffer** (`hn4_alloc_horizon`).
    *   Uses atomic cursor: `atomic_fetch_add(write_head, 1)`.
    *   Guarantees finding a slot in 1 cycle.
*   **Trade-off:** Sequential allocation (Linked List) prevents `ENOSPC` errors but loses parallel access benefits.

---

## 7. Performance Proof ($O(1)$)

1.  **No Tree:** Address calculation is `Multiply + Add + Modulo` ($\approx 4$ cycles).
2.  **No Scan:** Probes specific coordinates, does not scan for holes.
3.  **Bounded Retries:** Loop cap at 20 iterations (`HN4_MAX_PROBES`).
    *   Best Case: 1 Probe.
    *   Worst Case: 20 Probes.
    *   Fallback: 1 Atomic Add.
    *   Constant Bound = $O(1)$.

**Work per allocation does not increase with volume size or fragmentation.**

---

## 8. Hardware Acceleration

Optimizations for Bare Metal:
*   **SWAR:** SIMD Within A Register for ECC (`__builtin_parityll`).
*   **Branch Prediction:** `HN4_LIKELY` / `HN4_UNLIKELY` macros.
*   **Cache Locality:** Bitmap hierarchy fits in L2/L3 cache.

---

## 9. Probabilistic Mechanics

The allocator uses **Monte Carlo Localization** with a hard retry limit.

### 9.1 Collision Probability
For occupancy $P$, probability of collision is $P$. Probability of $K$ consecutive failures is $\epsilon = P^K$.

### 9.2 The "Rule of 20"
With $K=20$:

| Occupancy ($P$) | Success Probability | Failure Rate ($\epsilon$) | State |
| :--- | :--- | :--- | :--- |
| **50%** | 99.9999% | $9.5 \times 10^{-7}$ | **Ideal** |
| **75%** | 99.68% | $3.1 \times 10^{-3}$ | **Nominal** |
| **90%** | 87.84% | $12.15\%$ | **Saturated** |

### 9.3 Deterministic Fallback
At high load, the allocator abandons guessing and appends to the Horizon Log. This bounds latency even in worst-case scenarios.

---

## 10. Tradeoffs & Failure Modes

### 10.1 NVM Trust
*   **Risk:** `HN4_HW_NVM` bypasses software ECC. Relies on hardware reliability.
*   **Benefit:** 2x-3x higher IOPS.

### 10.2 Horizon Debt
*   **Consequence:** Horizon files are sequential. Reading them later may require random seeks if interleaved.
*   **Resolution:** Scavenger defragments Horizon blocks back to Primary Region (`_uptier_horizon_data`).

### 10.3 Adversarial Entropy
*   **Risk:** Hash DoS to induce collisions.
*   **Defense:** **Vector Mutation** makes trajectory non-linear at $K=4$, breaking prediction.

---

## 11. Profile-Specific Behaviors

### 11.1 AI (`HN4_PROFILE_AI`)
*   **Topology:** `_get_ai_affinity_bias` finds NVMe Namespace closest to GPU.
*   **Window:** Restricts $G$ to local NUMA node LBA range.

### 11.2 HDD (`HN4_DEV_HDD`)
*   **Inertial Damper:** Forces $V=1$ (Sequential).
*   **Orbit Lock:** Disables K-Shell search ($K>0$) to prevent seek thrashing. Fails to Horizon immediately.
*   **Warm Locality:** Biases $G$ near previous allocation.

### 11.3 ZNS (`HN4_DEV_ZNS`)
*   **Zone Lock:** Maps logical blocks to Zone Append commands.
*   **V-Force:** Forces $V=1$. Random writes are illegal.

---

## 12. Performance Visualizations

### 12.1 Tail-Latency vs. Occupancy
Comparing $O(1)$ Void Engine vs $O(N)$ Linear Scan. Note the "Cliff" at 90% where Horizon Fallback caps latency.

```text
Latency (µs)
  ^
  |                                        . (O(N) - Linear Scan)
50|                                      .
  |                                    .
40|                                  .
  |                                .
30|                              .
  |                            .
20|                          .
  |                        .
10|---------------------==[=================] (HN4 - Horizon Fallback)
  |                     .
 1|===================='                      (HN4 - Primary Region)
  +--------------------------------------------------> Occupancy (%)
  0        25       50       75       90       100
```

### 12.2 Entropy Distribution
Visualizing Genesis allocations ($K=0$) across address space.
`.` = Empty, `X` = Allocated.

```text
LBA 0                                                LBA MAX
[X...X..X....X..X...X.....X..X...X...X..X.....X...X..X...] (Uniform)
[XXXXXX...................XXXXXX..................XXXXXX.] (Clustered - Bad)
```

### 12.3 Probe Depth Histogram
Number of probes required for success. Ideally 99% at $K=0$.

```text
Count
  ^
  | [##############] (K=0: Primary - 98.5%)
  |
  | [##]             (K=1: First Collision - 1.2%)
  | [#]              (K=2: Second Collision - 0.2%)
  | [.]              (K=3..12: Rare - <0.1%)
  +----------------------------------------------------> K (Depth)
     0    1    2    3    4    8   12
```

### 12.4 GC Overhead vs. Horizon Usage
Scavenger CPU usage increases as Horizon fills.

```text
CPU %
  ^
  |                                     / (Panic Mode)
80|                                   /
  |                                 /
60|                               /
  |                             /
40|                           /
  |              ____________/
20|_____________/                     (Background Scrub)
  +--------------------------------------------------> Horizon Usage (%)
  0             50                    90            100
```

### 12.5 Allocation Jitter (Concurrency)
Latency variation with 64 threads. NVM (Lock-Free) vs Standard (CAS).

```text
Time (ms)
  ^
  |      |  |      |   (Standard SSD - CAS Contention)
10|  | | | || | | ||
  |  |||||||||||||||
  |
  |  ...............   (NVM - Atomic Fetch-Add)
 1|  ...............
  +--------------------------------------------------> Time
```

### 12.6 Crash Consistency Replay
Simulating power loss. `SEQ_CST` ensures Bitmap Claim is visible before Data Write.

```text
Timeline ->
Correct:   [Bitmap Claim (SEQ_CST)] ----> [Data Write] ----> [Metadata Commit]
                                      ^
                                   CRASH!
Replay:    Bitmap shows 'Used'. Data is garbage.
Result:    LEAK (Safe). Scavenger reclaims later.

Incorrect: [Store Buffer] (Bitmap Claim delayed)
           [Data Write] ----------------> [Metadata Commit]
                                      ^
                                   CRASH!
Replay:    Bitmap shows 'Free'. Metadata points to block.
Result:    DOUBLE ALLOC RISK (Fatal).
```

## 13. Safety Instrumentation

### 13.1 Impossible Free Detector
If `free()` is called but global usage counter is 0, an **Underflow** is detected.
*   **Action:** Clamp counter, Mark Volume `DIRTY`.
*   **Resolution:** Next mount triggers full bitmap audit.

### 13.2 Leak Preference
Design prefers "Leaking Space" over "Double-Allocating Data".
*   Crash after Claim, before Link -> Block is leaked.
*   Scavenger reclaims orphans later.
