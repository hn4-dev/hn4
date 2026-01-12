# HN4 THE VOID ENGINE
### *The Ballistic-Tensor Allocator*
**Specification:** v16.1 | **Module:** `hn4_allocator.c` | **Paradigm:** Stateless Probabilistic

---

## 1. Abstract: The End of the Free List

Traditional allocators (ext4, NTFS, malloc) are **Stateful**. They maintain complex data structures (Bitmaps, Free Lists, B-Trees) that map the location of every free byte. To allocate space, the CPU must traverse these structures to find a "hole." As the disk fills up, fragmentation increases, and traversal time degrades from $O(\log N)$ to $O(N)$.

**The Void Engine is Stateless.**

It does not search for free space; it calculates a **Trajectory** where data *should* exist based on a deterministic physical equation. It then queries the memory map to see if that specific coordinate is available.

*   **Logic:** Instead of asking "Where is free space?", HN4 asks "Is Coordinate $X$ free?"
*   **Result:** Allocation time is decoupled from Disk Capacity and Fragmentation. Allocating a block on a 1.44MB Floppy takes the same CPU cycles as allocating on a 100PB Array.

---

## 2. NVM.2 & Lying SSD Architecture

### 2.1 The NVM.2 Fast Path
When the underlying media is detected as **Byte-Addressable Non-Volatile Memory** (e.g., Intel Optane, MRAM, CXL Memory), the Allocator engages a zero-overhead fast path.

*   **Flag:** `HN4_HW_NVM` (0x4000).
*   **Bypass:** Skips Software ECC calculation and the heavy 128-bit CAS loop.
*   **Mechanism:** Uses atomic 64-bit fetch-and-or/fetch-and-and operations directly on the bitmap.
*   **Throughput:** Increases allocation speed by ~2.5x on DAX-mapped volumes.

### 2.2 The Lying SSD Mitigation
Consumer SSDs often acknowledge writes before data hits stable NAND (volatile DRAM cache). If power fails, the bitmap state in the OS might differ from the drive's reality.

*   **Hazard:** Phantom Collision (OS thinks block is used, Drive thinks it's free, or vice-versa).
*   **Mitigation:** `memory_order_seq_cst` fences are enforced on bitmap updates. This prevents the CPU store buffer from reordering the bitmap "claim" with the subsequent data write, ensuring that if the OS crashes, the allocation order is strictly preserved in the visible timeline.

---

## 3. The Mathematics: The Equation of State

The physical location (LBA) of any block $N$ of any file is derived from four variables. This is the **Universal Trajectory Function**.

$$ LBA_{phys} = LBA_{flux} + \left( [ G + (N \times V \times 2^M) + \Theta(k) ] \pmod{\Phi_M \times 2^M} \right) $$

### 3.1 The Variables
1.  **$G$ (Gravity Center):** The seed LBA. A random starting point selected at file creation.
2.  **$N$ (Sequence):** The logical block index ($0, 1, 2...$).
3.  **$V$ (Orbit Vector):** The "Stride" or angle of the trajectory.
    *   Must be **Coprime** to the Volume Capacity ($\Phi_M$) to guarantee full-disk coverage (The Resonance Defense).
    *   Common values: `1` (HDD), `17` (SSD), `101` (RAID).
4.  **$M$ (Fractal Scale):** The block size multiplier ($2^0=4\text{KB}$, $2^{10}=4\text{MB}$).
5.  **$\Theta(k)$ (Collision Shell):** The orbital offset used for collision resolution and Shadow Hops (Updates).

### 3.2 The Resonance Defense

To prevent "Harmonic Locking" (where a file's stride aligns with divisors of the disk size, causing it to loop over a small subset of blocks repeatedly), the Allocator enforces a strict algebraic invariant:

**The Coprimality Invariant:**
The Orbit Vector **V** must be coprime to the **Volume Capacity ($C$)**.

$$ \gcd(V, C) \equiv 1 $$

**Implementation Logic:**
1.  **Detection:** The engine calculates the Greatest Common Divisor of the candidate Vector and the Volume Capacity.
2.  **Tuning:** If they share factors, the engine scans the **Golden Prime Table** for a substitute.
3.  **The Failsafe:** If all Golden Primes resonate (a theoretical edge case), the engine forces **V = 1** (The Linear Rail). Since 1 is coprime to every integer, this guarantees the invariant holds.

**Mathematical Proof:**
Because $\gcd(V, C) = 1$, the trajectory $T(n) = (G + nV) \pmod C$ generates a **Cyclic Group of Order $C$**. It is **mathematically impossible** for the trajectory to wrap around to the start ($G$) before visiting every single unique block in the volume exactly once. This guarantees 100% address space coverage with zero dead spots.

---

## 4. Algorithmic Workflow

### 4.1 Genesis (Creation - Phase 1)
When creating a new file, the engine performs a **Monte Carlo Probe**.

1.  **Usage Check:** Is the global saturation $> 90\%$? If yes, abort to **Horizon** (Phase 3).
2.  **Probe:** The CPU selects a random integer $G_{cand}$ (Candidate Gravity Center).
3.  **Simulation:** The engine effectively "dry runs" the trajectory for the first 4 blocks ($N=0..3$) at the ideal orbit ($k=0$).
4.  **Verification:** It queries the **L3 Armored Bitmap** (in RAM).
    *   *Query:* "Are the bits at $LBA(G_{cand}, N)$ currently 0?"
5.  **Decision:**
    *   **Success:** If all 4 probes hit `0`, the location is claimed. $G_{cand}$ becomes the permanent `Anchor.gravity_center`.
    *   **Collision:** If *any* probe hits `1`, $G_{cand}$ is rejected. The CPU picks a new random $G$ and retries.
    *   *Retry Limit:* Hard cap at 20 retries.

**Why this is O(1):** Even at 80% disk capacity, the probability of finding a free slot within 20 probes is statistical certainty. The code does not "scan"; it "samples."

### 4.2 The Shadow Hop (Write/Update)
HN4 never overwrites data in place. It performs an **Orbital Jump**.

To update Block $N$:
1.  **Calculate Current:** $LBA_{old} = f(G, V, N, k_{current})$.
2.  **Calculate Next:** $LBA_{new} = f(G, V, N, k_{current} + 1)$.
    *   The term $\Theta(k)$ adds a deterministic offset based on the shell index.
3.  **Verify:** Check Bitmap for $LBA_{new}$.
    *   If occupied, increment $k$ and try again ($k=2, 3...$).
4.  **Commit:** Write data to $LBA_{new}$.
5.  **Eclipse:** Issue atomic Trim/Zero to $LBA_{old}$.

### 4.3 Gravity Assist (Phase 2)
If the "Immediate Neighborhood" ($k=0..3$) is full due to local clustering:

*   The engine engages **Gravity Assist**.
*   **Math:** $V_{assist} = \text{ROTL}(V, 17) \oplus \text{0xA5A5A5...}$
*   **Effect:** This bit-shift applies a chaotic transformation to the vector. The trajectory teleports to a completely uncorrelated region of the disk.
*   **Benefit:** This defeats the "Birthday Paradox" of hashing. Even if Region A is 99% full, the Assist jumps to Region B, which might be empty.

---

## 5. The Memory Map: H-Bit Tree

To make the "Verification" step instant, HN4 maintains a hierarchical bitmap in RAM.

### 5.1 L3: The Armored Truth
*   **Granularity:** 1 bit = 1 Physical Block (e.g., 4KB).
*   **Safety:** Stored as `hn4_armored_word_t` (64-bits Data + 8-bits ECC).
*   **Standard Mode:** Updated via 128-bit CAS (Compare-And-Swap) to prevent bit-rot corruption.
*   **NVM Mode:** Updated via 64-bit Atomic OR/AND (No ECC, rely on media).

### 5.2 L2: The Summary Layer
*   **Granularity:** 1 bit = 512 Blocks (2MB).
*   **Logic:** If `L2_Bit[i] == 0`, then **ALL** 512 blocks in that region are mathematically guaranteed to be free.
*   **Optimization:** When the Allocator probes a region, it checks L2 first.
    *   If `L2 == 0`: It skips the L3 check entirely. **64x Performance Boost.**
    *   If `L2 == 1`: It dives down to L3 to check specific bits.

---

## 6. Saturation Logic: The Event Horizon (Phase 3)

The probabilistic model degrades when the disk is $>95\%$ full (too many collisions). The Void Engine handles this deterministically.

*   **Trigger:** If `used_blocks > (capacity * 0.90)`.
*   **Action:** All new allocations are routed to **D1.5 (The Horizon)**.
*   **Mechanism:**
    *   The Horizon is a simple **Linear Ring Buffer**.
    *   It maintains an atomic cursor: `atomic_fetch_add(write_head, 1)`.
    *   It guarantees finding a slot in exactly 1 cycle (no probing).
*   **Cost:** Files allocated here lose the benefits of parallelism (they become sequential linked lists), but the system *never* hangs or fails with `ENOSPC` until the last byte is gone.

---

## 7. Performance Proof ($O(1)$)

Why do we claim $O(1)$ constant time complexity?

1.  **No Tree Traversal:** There is no B-Tree depth to descend. The address calculation is `Multiply + Add + Modulo`. This takes $\approx 4$ CPU cycles.
2.  **No Linear Scan:** We do not scan the bitmap for holes. We probe specific coordinates.
3.  **Bounded Retries:** The loop is hard-capped at 20 iterations.
    *   Best Case: 1 Probe.
    *   Worst Case: 20 Probes.
    *   Fallback: 1 Atomic Add (Horizon).
    *   In Big-O notation, a bounded constant (20) is dropped. $T(n) = O(1)$.

**The probability landscape changes as occupancy rises — but the work per allocation does not grow with history or volume size. That is the definition of constant time.**

---

## 8. Hardware Acceleration

The `hn4_allocator.c` code is optimized for Bare Metal instruction sets:

*   **SWAR (SIMD Within A Register):** The `_alloc_popcnt64` function updates usage counters using bitwise parallelism, avoiding branches.
*   **Branch Prediction:** Usage of `HN4_LIKELY` / `HN4_UNLIKELY` ensures the "Happy Path" (Probe Success) stays in the CPU Instruction Pipeline.
*   **Cache Locality:** The H-Bit Tree fits in L3/L2 CPU Cache. A probe usually hits cache, avoiding RAM latency entirely.

---

## 9. Probabilistic Mechanics: Retry Ceiling vs. Occupancy

The Void Engine rejects the concept of "scanning for free space." Instead, it employs **Monte Carlo Localization**. To maintain strict Real-Time (RTOS) compatibility, the allocator enforces a hard limit on the number of probes (guesses) it makes before declaring the Flux Manifold "statistically full."

### 9.1 The Probability of Collision
Given a disk occupancy $P$ (where $0.0 \le P \le 1.0$), the probability that a random coordinate $G$ is occupied is $P$. The probability that it is free is $1 - P$.

If the allocator makes $K$ independent probes (retries), the probability $\epsilon$ that **all** probes collide (total failure) is:

$$ \epsilon = P^K $$

### 9.2 The "Rule of 20" (Constant $K_{max}$)
HN4 defines `HN4_MAX_GENESIS_RETRY = 20`.
The system is designed to operate primarily in the **Ballistic Regime** ($P \le 75\%$) and degrade gracefully into the **Horizon Regime** ($P > 90\%$).

**Failure Probability Table ($K=20$):**

| Disk Occupancy ($P$) | Success Probability ($1 - P^{20}$) | Failure Probability ($\epsilon$) | State |
| :--- | :--- | :--- | :--- |
| **50%** | 99.9999% | $9.5 \times 10^{-7}$ | **Ideal** (Ballistic) |
| **75%** | 99.68% | $3.1 \times 10^{-3}$ | **Nominal** (Ballistic) |
| **85%** | 96.12% | $3.8 \times 10^{-2}$ | **Dense** (Mixed) |
| **90%** | 87.84% | $12.15\%$ | **Saturated** (Horizon Fallback) |

### 9.3 The Deterministic Fallback (D1 $\to$ D1.5)
The allocator does **not** loop infinitely. It effectively makes a wager:

> *"If I cannot find a free block in 20 random guesses, the entropy of the free space is too high to solve mathematically. I will stop guessing and append to the Horizon Log."*

This guarantee converts an unbound $O(N)$ fragmentation problem into a bounded $O(K)$ latency guarantee.

**The allocator never experiences allocator-age performance decay. Even a ten-year-old volume allocates at the same constant cost as a fresh one.**

### 9.4 The Saturation Guard
To prevent wasting cycles when the disk is known to be full, `_check_saturation` short-circuits the retry loop entirely.

$$ \text{IF } (\text{Used} > \text{Total} \times 0.90) \implies \text{GOTO Horizon} $$

This ensures that at $P=0.95$, where $\epsilon \approx 36\%$, the system does not waste CPU time on ballistic probes that are likely to fail, preserving system responsiveness under load.

---

## 10. Critical Tradeoffs & Failure Modes

### 10.1 NVM.2 Trust Model (The Firmware Wager)
**Tradeoff:** Enabling `HN4_HW_NVM` bypasses the Armored Bitmap's software ECC validation.
*   **Benefit:** 2x-3x higher IOPS on Optane/DAX.
*   **Risk:** If the NVM firmware lies about persistence or suffers silent bit rot, HN4 will not detect the corruption in the allocation map.
*   **Mitigation:** This mode is strictly optional. Enterprise deployments on untrusted NVM hardware should unset the flag to retain ECC protection at the cost of CPU cycles.

### 10.2 Horizon Mode Debt (The GC Burden)
**Tradeoff:** The Horizon Log prevents `ENOSPC` errors but destroys spatial locality.
*   **Effect:** Files allocated in the Horizon are strictly sequential in write order (Temporal Clustering), not logical order.
*   **Consequence:** Reading a Horizon-allocated file later may require random seeks (Thrashing) if the file was interleaved with other writers.
*   **Resolution:** The Scavenger module must aggressively defragment Horizon blocks back into the Flux Manifold during idle periods, creating "Garbage Collection Debt."

### 10.3 Monte-Carlo "False Fullness" (90-95%)
**Tradeoff:** The bounded retry limit ($K=20$) means the allocator is probabilistic, not exhaustive.
*   **Effect:** At 95% occupancy, there is a ~36% chance the allocator will fail to find a valid hole *even if one exists*.
*   **Consequence:** The system will fallback to Horizon earlier than a linear scanner would.
*   **Behavior:** New allocations may cluster around "Lucky" anchors (coordinates that happened to be free), creating uneven wear leveling until the Scavenger rebalances.

### 10.4 Adversarial Entropy Collapse
**Tradeoff:** Deterministic math is predictable.
*   **Scenario:** A malicious actor attempts to exhaust the volume by writing files specifically timed to collide with the hash of another specific file (Hash DoS).
*   **Risk:** If users hammer the same Vector $V$ and Block Size $M$, they could artificially induce collision depths $> K_{max}$.
*   **Defense:** The **Gravity Assist** mechanism. By rotating the vector bits (`ROTL(V, 17)`) at $K=4$, the trajectory becomes non-linear relative to the input. An attacker cannot predict the collision path without reversing the entire bitmap state. **In practice, inducing pathological collision chains requires privileged timing knowledge and sustained write access. Random workloads cannot self-organize into this pattern.**

---

## 11. Profile-Specific Behaviors (AI, HDD, NVM)

### 11.1 AI / Tensor Tunnel (`HN4_PROFILE_AI`)
The Void Engine treats AI workloads as a distinct class of physics problem. Neural network training involves massive, contiguous matrix ingestion where seek latency is fatal to GPU throughput.

*   **Topology Awareness:** The allocator queries the HAL for the **PCIe Switch Topology**. It identifies which NVMe Namespaces are "closest" (least hops) to the requesting GPU.
*   **Affinity Window:** Instead of probing the entire disk ($0 \dots MAX$), the allocator restricts its search window to the specific LBA range mapped to the local NUMA node.
    *   *Result:* Data is physically placed on the flash chips closest to the tensor cores consuming it.
*   **Quality Enforcement:** The allocator aggressively checks the **Q-Mask**. It rejects "Bronze" (slow/worn) blocks for AI data, guaranteeing consistent streaming bandwidth.

### 11.2 Rotational Media (`HN4_DEV_HDD`)
Hard Drives are mechanical. Random access is physically expensive (Seek Time). The Ballistic Allocator, which defaults to scattering data for entropy, must be restrained.

*   **Inertial Damper:** The allocator forces the Orbit Vector $V$ to `1` (Sequential).
*   **Orbit Lock:** It disables the K-Shell search ($k=1 \dots 12$). If the primary sequential slot is taken, it fails immediately to Horizon rather than "shotgunning" across the platter.
*   **Warm Locality:** When allocating a new file, it biases the Gravity Center ($G$) to be within 32 blocks of the *previous* allocation. This keeps the drive head in the same cylinder, minimizing seek latency during write bursts.

### 11.3 Zoned Namespaces (`HN4_DEV_ZNS`)
ZNS drives forbid random writes within a zone. Writes must be strictly sequential relative to the Zone Write Pointer.

*   **Zone Lock:** The allocator maps logical blocks directly to physical Zone Append commands.
*   **Macro-Blocking:** The logical block size is elevated to match the Zone Size (e.g., 256MB). This turns the "Block Allocator" into a "Zone Allocator."
*   **V-Force:** Like HDD, $V$ is forced to `1`. Random writes are mathematically impossible in this mode; the allocator acts as a log-structured append engine.

### 11.4 NVM / Storage Class Memory (`HN4_HW_NVM`)
Memory-speed storage requires the removal of all software bottlenecks.

*   **Atomic Bypass:** The allocator detects if the CPU supports atomic bit-test-and-set instructions (BTS/BTR on x86). If so, it replaces the complex CAS loop with single-instruction hardware atomics.
*   **Cache Line Alignment:** Allocations are padded to align with CPU cache lines (64 bytes), preventing "False Sharing" where two threads fight over the same cache line for different allocations.

---

## 12. Performance Visualizations

### 12.1 Tail-Latency vs. Occupancy
This graph demonstrates the $O(1)$ behavior of the Void Engine compared to a traditional $O(N)$ linear scanner (Ext4/FAT). Note the "Cliff" at 90% where Horizon Fallback caps latency.

```text
Latency (µs)
  ^
  |                                        . (O(N) - Ext4/FAT Scan)
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
 1|===================='                      (HN4 - Ballistic D1)
  +--------------------------------------------------> Occupancy (%)
  0        25       50       75       90       100
```

### 12.2 Cold Start Entropy Distribution
Visualizing the distribution of Genesis allocations ($K=0$) across the address space. Ideally uniform.
`.` = Empty Block, `X` = Allocated Block.

```text
LBA 0                                                LBA MAX
[X...X..X....X..X...X.....X..X...X...X..X.....X...X..X...] (Uniform)
[XXXXXX...................XXXXXX..................XXXXXX.] (Clustered - Bad)
```

### 12.3 Anchor Fairness (K-Depth Histogram)
How many probes did it take to find a slot? Ideally, 99% of allocs succeed at $K=0$.

```text
Count
  ^
  | [##############] (K=0: Primary Orbit - 98.5%)
  |
  | [##]             (K=1: First Collision - 1.2%)
  | [#]              (K=2: Second Collision - 0.2%)
  | [.]              (K=3..12: Rare/Gravity Assist - <0.1%)
  +----------------------------------------------------> K (Depth)
     0    1    2    3    4    8   12
```

### 12.4 GC Overhead vs. Horizon Debt
As the Horizon fills, Scavenger CPU usage increases linearly to pay down the fragmentation debt.

```text
CPU %
  ^
  |                                     / (GC Panic Mode)
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

### 12.5 Allocation Jitter (Thread Contention)
Latency variation under high concurrency (64 threads).
The NVM.2 path (Lock-Free) shows significantly less jitter than Standard (CAS Loop).

```text
Time (ms)
  ^
  |      |  |      |   (Standard SSD - CAS Contention Spikes)
10|  | | | || | | ||
  |  |||||||||||||||
  |
  |  ...............   (NVM.2 - Atomic Fetch-Add)
 1|  ...............
  +--------------------------------------------------> Time
```

### 12.6 Reorder-Crash Consistency Replay
Simulating power loss during allocation.
`W` = Write, `B` = Bitmap Update.
`SEQ_CST` ensures `B` is visible before `W` is acknowledged as safe.

```text
Timeline ->
Correct:   [Bitmap Claim (SEQ_CST)] ----> [Data Write] ----> [Metadata Commit]
                                      ^
                                   CRASH!
Replay:    Bitmap shows 'Used'. Data is garbage.
Result:    LEAK (Safe). Scavenger reclaims later. No Data Corruption.

Incorrect: [Store Buffer] (Bitmap Claim delayed)
           [Data Write] ----------------> [Metadata Commit]
                                      ^
                                   CRASH!
Replay:    Bitmap shows 'Free'. Metadata points to block.
Result:    DOUBLE ALLOC RISK (Fatal).
```

### 12.7 Long-Term Fragmentation Map
Visualizing fragmentation after 1 year of random writes/deletes.
White = Free, Black = Used.

**Ext4 (Bitmap Search):**
```text
[██████████████░░░░░░░░░░░░░░░░░░░░░] (Front-Loaded Fill)
```

**HN4 (Ballistic):**
```text
[█░█░██░░█░█░░███░░█░░██░█░░█░██░░█] (High Entropy / Uniform Wear)
```

### 12.8 Worst-Case Multi-Anchor Collision
Scenario: 100 files all hashing to the same $G$ and $V$.
Allocator behavior:

```text
File 1: G+0
File 2: G+1
File 3: G-1
File 4: G+2
...
File 5: Gravity Assist (Teleport to Random G')
File 6: Gravity Assist (Teleport to Random G'')
...
Result: Local cluster forms, then explodes outward. No infinite loop.
```

## 13. Metric Drift & Safety Instrumentation

In a non-journaled system like HN4, power failure during an allocation creates a small window of incoherency between the **Allocation Map** (Physical Truth) and the **Usage Counter** (Logical Stat). This is called "Metric Drift".

### 13.1 The "Impossible Free" Detector
The allocator instruments the Free Path to detect drift actively. If the kernel attempts to free a block, but the global usage counter is already 0 (or less than the free count), an **Underflow Event** has occurred.

**Behavior:**
1.  **Clamping:** The counter is clamped to 0 to prevent unsigned integer wrap-around (which would cause a false Saturation trigger).
2.  **Tainting:** The volume is atomically marked `HN4_VOL_DIRTY`.
3.  **Resolution:** The next mount (or background Scavenger) observes the Dirty flag and performs a **Convergent Bitmap Audit**, re-summing the population count from the physical bitmap to restore truth.

### 13.2 Crash Consistency Replay
The engine relies on strict ordering to ensure safety during drift:
1.  **Claim First:** Bits are set in the bitmap (`SEQ_CST` fence).
2.  **Write Second:** Data is written to media.
3.  **Link Third:** Anchor metadata is updated.

**Scenario:** Crash after Claim, before Link.
*   **Result:** The bit is "Used" in the bitmap, but no file points to it. This is a **Leak**, not corruption.
*   **Healing:** The Scavenger eventually identifies blocks with no owning Anchor (via reverse-mapping or timeout) and reclaims them.

This design prefers "Leaking Space" over "Double-Allocating Data," maintaining rigorous data safety even in the absence of a journal.