# HN4: The Ballistic-Tensor Manifold & HDD Optimization Strategy
**Filename:** `hn4_hdd.md`
**Version:** 2.1 (Media-Adaptive Revision)
**Status:** IMPLEMENTED

---

## 1. Executive Summary

**Hydra-Nexus 4 (HN4)** is a "Post-POSIX" storage engine designed primarily for high-velocity NVMe environments. It replaces traditional allocation structures (B-Trees, inode tables, extents) with a ballistic addressing model that uses quadratic equations to compute physical data location in $O(1)$ time.

While this randomization pattern is ideal for SSDs, it is highly sub-optimal for **Hard Disk Drives (HDDs)**. On rotational media, the scattered access pattern forces excessive seek operations. Because seek latency dominates mechanical performance, throughput can collapse by more than 90% when using standard ballistic logic.

**Media-Adaptive Architecture:**
HN4 solves this by dynamically adapting to the characteristics of the underlying storage device. The allocator and reader alter their behavior based on detected hardware flags (`HN4_HW_ROTATIONAL`), allowing HN4 to act like **two optimized storage engines under a unified API**: a Tensor Store for SSDs and a Linear Stream Store for HDDs.

This document details the **Inertial Damper (Spec 10.8)**, the specific logic path implemented to align HN4 with the physics of rotational media.

---

## 2. Core Architecture: How HN4 Works

To understand the optimization, one must understand the "Gravity Well" architecture.

### 2.1 The Equation of State
In HN4, a file is not a linked list. It is a mathematical equation. To find a block of data, the CPU calculates:

$$ LBA = G + (N \times V) + \Theta(K) $$

*   **$G$ (Gravity Center):** The starting physical sector.
*   **$N$ (Sequence):** The logical block number (0, 1, 2...).
*   **$V$ (Orbit Vector):** A Prime Number stride (e.g., 17, 1021).
*   **$K$ (Trajectory):** Collision offset (0..12).

### 2.2 The SSD Strategy (Standard)
On an SSD, HN4 sets $V$ to a large prime number (e.g., 1021).
*   **Result:** Block 0 is at $G$. Block 1 is at $G+1021$. Block 2 is at $G+2042$.
*   **Why?** The stride distributes writes across independent NAND channels and dies, increasing internal parallelism and effective throughput.
*   **Collision:** If a spot is taken, it increments $K$ (Gravity Assist), jumping to a pseudo-random location to avoid wear hotspots.

---

## 3. The HDD Problem: "Mathematical Thrashing"

If the standard SSD logic is applied strictly to a mechanical drive:

1.  **The Stride Issue:** With $V=1021$, reading a 1MB file requires the drive head to seek 1021 sectors forward for *every single 4KB block*.
    *   *Result:* Throughput drops to ~3 MB/s.
2.  **The Shotgun Issue:** To find data instantly, HN4 usually issues 4 reads in parallel ($K=0, 1, 2, 3$). On an HDD, this forces the head to seek to 4 different locations to read 1 block.
    *   *Result:* Latency spikes from 10ms to 40ms+.

---

## 4. The Solution: Inertial Damping (Spec 10.8)

The code has been modified to detect `HN4_HW_ROTATIONAL` (HDD) or `HN4_PROFILE_ARCHIVE`. When active, the physics engine changes modes.

### 4.1 "The Railgun" (Write Optimization)
**Source:** `hn4_allocator.c` / `hn4_alloc_genesis`

Instead of creating a "Cloud" of data, we create a "Rail."

*   **Force Vector $V=1$:** The allocator ignores the Prime Number stride. It forces $V=1$.
    *   *Math:* $LBA = G + (N \times 1)$.
    *   *Physicality:* Block 1 is physically adjacent to Block 0. The HDD head does not move.
*   **Disable Gravity Assist:** The allocator only checks Trajectory $K=0$.
    *   If $LBA(K=0)$ is occupied, it **DOES NOT** try to jump to a random location ($K=1$).
    *   **Action:** It immediately fails over to the **Horizon (D1.5)**.
*   **The Horizon Fallback:** The Horizon is an always-appendable linear extent reserved for overflow and relocation. It acts as a safety valve to guarantee sequential writes when the primary rail is blocked.

### 4.2 "The Sniper" (Read Optimization)
**Source:** `hn4_read.c` / `hn4_read_block`

We disable the speculative "Shotgun" behavior.

*   **Logic:**
    ```c
    /* HDD OPTIMIZATION */
    uint8_t k_limit = is_hdd ? 1 : HN4_MAX_TRAJECTORIES;
    ```
*   **Behavior:** The driver reads **only** the ideal mathematical location ($K=0$). It waits for the result.
*   **Result:** The drive queue depth remains low (1-2), allowing the HDD's internal elevator algorithms (NCQ) to work efficiently.

### 4.3 "Next-Fit" Allocation
**Source:** `hn4_allocator.c`

*   **Standard:** Monte Carlo (Random) probing to reduce NAND wear leveling.
*   **HDD Mode:** Linear Scanning.
    *   *Logic:* If the drive head is at LBA 1,000,000, try to allocate the next file at LBA 1,000,001.
    *   *Benefit:* Keeps the write cursor moving forward across the platter, **treating the HDD almost like a sequential tape device.**

---

## 5. Technical Implementation Details

### The Modified Allocator Function
In `hn4_alloc_genesis`, the Vector selection logic was altered:

```c
/* HDD OPTIMIZATION: Inertial Damper (Spec 10.8) */
bool is_hdd = (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) || 
              (vol->sb.info.format_profile == HN4_PROFILE_PICO);

if (is_hdd) {
    /* Force Sequential Trajectory to minimize head seeking */
    v_final = 1;
}
```

And the collision loop was clamped:

```c
/* HDD OPTIMIZATION: Only probe K=0. */
int n_limit = is_hdd ? 1 : 4;
```

### The Modified Reader Function
In `hn4_read_block`, the trajectory loop was restricted:

```c
/* 
 * HDD Logic: Disable Shotgun. 
 * We limit to k=1 (Orbit 0 only). 
 */
uint8_t k_limit = (is_linear || is_hdd) ? 1 : HN4_MAX_TRAJECTORIES;
```

---

## 6. Benchmarks (Simulated)

**Environment:** 4TB 7200RPM HDD (SATA 6Gb/s).

### Scenario A: Writing a 10GB Movie File
*   **Standard HN4 (SSD Logic):**
    *   Writes 4KB chunks scattered by Prime Stride 1021.
    *   Drive must seek every 4KB.
    *   **Speed:** ~3.5 MB/s. (Unusable).
*   **Optimized HN4 (Inertial Mode):**
    *   Writes contiguous stream ($V=1$).
    *   Drive performs continuous platter rotation.
    *   **Speed:** ~180 MB/s. (**Saturation**).

### Scenario B: Reading a Directory of Images
*   **Standard HN4 (SSD Logic):**
    *   Issues 4 speculative reads per file.
    *   Total Seeks for 100 files: 400.
    *   **Time:** ~4.8 seconds.
*   **Optimized HN4 (Inertial Mode):**
    *   Issues 1 precise read per file.
    *   Total Seeks for 100 files: 100.
    *   **Time:** ~1.2 seconds.

---

## 7. Conclusion

By implementing the **Inertial Damper**, HN4 demonstrates true media-adaptability. It transforms from an "SSD-Only" exotic filesystem into a universal storage substrate.

The system retains the metadata benefits (Tags, Tethers, $O(1)$ lookup) while respecting the physical limitations of rotational media. It behaves as a **Random-Access Tensor Store** on SSDs and a **Linear Tape/Stream Store** on HDDs, switching logic automatically at mount time.