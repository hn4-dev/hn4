# Hydra-Nexus 4 — Changelog

All notable changes to **HN4 (Hydra-Nexus 4)** will be documented in this file.

This project follows a stability-first philosophy: correctness, durability,
and observability take priority over features. Entries below describe
externally-visible behavior, storage-format semantics, and major internal
architecture shifts.

---

## [0.1.4] — 2026-01-08
### Allocator Physics, Fragmentation Resilience & Consistency Guarantees

This release focuses on the allocator as a mathematical system — not a best-effort heuristic. I introduced physics-style validation, fragmentation stress tests, concurrency fuzzing, and hard safety invariants so that allocation, free, and saturation behavior remain correct — even under pathological workloads.

The outcome: allocator decisions are now formally test-verified rather than assumed.

### 🧠 Allocator Physics & Scatter Logic

HN4’s allocator deliberately fragments writes to exploit device-level parallelism. This release validates that scatter math remains correct across vector modes and collision-escape logic.

#### Ballistic Trajectory Validation
Verified the fundamental trajectory equation
`LBA = FluxStart + φ(G,V,N)`
including prime-vector scatter spacing and φ-alignment behavior.
Ensures Block(N+1) is not adjacent unless intended.

#### Gravity-Assist Teleportation (Collision Escape)
Validated K≥4 vector mutation paths, ensuring the allocator “teleports” away from local density wells instead of clustering.
Confirmed non-linear jumps and forward-progress guarantees.

### 🧩 Fragmentation Resilience

Real disks fragment. This release proves HN4 continues allocating safely and predictably when the world looks like Swiss cheese.

#### Checkerboard Stress Simulation
Create contiguous allocations -> Free every other block -> Re-allocate repeatedly.
Verify:
- no probe-loop starvation
- no double-alloc
- no free-leak
- logical vs physical allocation parity holds

### 🌅 Event Horizon Saturation Logic

Validated the 90% saturation safety rail:
- When usage ≥ threshold, ballistic allocation halts
- The volume transitions to Horizon Append Mode
- A sticky runtime flag prevents oscillation
- Writes become log-structured & predictable
- Metadata integrity takes precedence over throughput

This ensures latency remains bounded under extreme pressure instead of degrading into chaos.

### ⚙️ ZNS Sequential-Write Enforcement

For HN4_DEV_ZNS devices:
- Vector stride (V) is now forced to 1
- Ensures sequential-only allocation semantics
- Prevents illegal randomization into Zoned Write Pointers

This applies uniformly across allocator entry points.

### 🔍 Consistency Invariants — Now Enforced

We added a hard-audit invariant that must always hold:
`used_blocks == popcount(bitmap)`

Every fuzz run ends with a full bitmap scan to validate that logical counters and physical reality agree.
If they ever diverge, tests fail. So far — they don’t.

### 🔥 Concurrency Storm Fuzzing (Deterministic & Portable)

New Monte-Carlo allocator fuzzing:
- 16 concurrent threads
- Allocate / Free / Horizon-Append mixes
- Deterministic portable RNG
- Immediate correctness checks
- End-state bitmap reconciliation
- Panic + taint flag monitoring

Goal: prove allocator correctness under real-world scheduler chaos — not just cleanroom conditions.
Result: No double-alloc, no ghost frees, no panic triggers, no accounting drift.

### 🧪 Cartography & Epoch Safety (Complementary Suite)

Additional verification landed:
- Quality-Tier Enforcement: Metadata rejects Bronze, Gold accepted universally, Toxic is globally banned
- Epoch Drift Detection: Future-epoch toxicity, rollback detection, ring wrap correctness, CRC validation paths. These tests reinforce anti-rollback durability guarantees.

---

## [0.1.3] — 2026-01-08
### **Recovery Hardening & Legacy Hardware Support**

This release focuses on forensic recovery capabilities, ensuring data can be salvaged from volumes with severe structural damage (Partition Wipes, Root Rot, Epoch Loss) while enforcing strict read-only quarantines to prevent historical tampering. It also validates operation on constrained legacy hardware.

### 🛡️ Recovery & Security Enhancements

#### **Forensic Mount Mode (Safety First)**
- **Epoch Loss Quarantine:** Modified the mount logic to allow mounting a volume even if the **Epoch Ring** (Time Journal) is destroyed or zeroed (e.g., after a partition table wipe).
  - **Behavior:** The volume now mounts in **Read-Only / Panic Mode**.
  - **Rationale:** Previously, this was a hard failure (`HN4_ERR_EPOCH_LOST`), making data salvage impossible. Now, forensics tools can extract files, but write operations are strictly forbidden to prevent "Phantom Replay" attacks or ordering violations.

#### **Southbridge Rescue Protocol**
- **Partition Wipe Recovery:** Verified and hardened the "Southbridge" logic. If the primary (North) Superblock and Epoch Ring are wiped (common in accidental `fdisk` or `dd` errors), the driver successfully locates and mounts using the **South Superblock** (End of Disk Backup).
- **Pico-Rescue:** The recovery path automatically engages **Pico Profile** logic (bypassing heavy bitmap loads) if the metadata regions are found to be zeroed/corrupt, ensuring access to raw file data even when allocation maps are lost.

#### **Dynamic Geometry Probing**
- **ZNS / Huge Block Support:** Fixed a critical flaw in the Cardinal Vote (Quorum) logic where it failed to find mirrors on volumes with non-standard block sizes (e.g., 64MB or 256MB ZNS Zones). The driver now dynamically probes mirror locations based on the block size discovered in the primary Superblock, enabling recovery on Hyperscale/AI storage arrays.

### 🏛️ Legacy & Embedded Support

#### **486 / Pentium / Cortex-M (No FPU/SSE)**
- **Software CRC Fallback:** Verified correct operation on CPUs lacking hardware CRC32 instructions. The driver correctly falls back to the Slicing-by-8 software algorithm without corruption.
- **Atomic Emulation:** Validated the persistence barrier logic on architectures lacking `CLFLUSH` or `MFENCE`. The HAL now correctly degrades to generic atomic operations, ensuring durability on vintage hardware (i486) and low-power wearables (Samsung Watch / ESP32).

### 🧪 Test Suite Additions
- **`Epoch.Ouroboros_Wrap`**: Verifies the Epoch Ring pointer correctly wraps around the physical ring boundary without off-by-one errors.
- **`Recovery.Root_Anchor_Regeneration`**: Validates the "Genesis Repair" logic—automatically regenerating a missing Root Directory inode if the volume is mounted R/W.
- **`Hardware.Profile_Permutations_Fixed`**: A massive regression test cycling through SSD, HDD, USB, and ZNS profiles to ensure geometry calculations hold up under 4GB+ capacities.

---

## [0.1.2] — 2026-01-07
### **ZNS Logic Hardening & Test Suite Expansion**

This release fixes a logic error in the Cardinal Vote unmount sequence for Zoned Namespaces and expands the test suite to cover catastrophic edge cases, geometry underflows, and security bounds.

### 🛡️ Core Logic Fixes

#### **ZNS (Zoned Namespaces)**
- **Quorum Relaxation:** Fixed a logic bug in `hn4_unmount` where the Cardinal Vote quorum required 2 successful writes (North + 1 Mirror). Since ZNS devices physically prevent rewriting mirror locations in Sequential Zones, the quorum logic now accepts "North-Only" success for devices with the `HN4_HW_ZNS_NATIVE` flag. This fixes the false-positive `HN4_ERR_HW_IO` during ZNS unmount.

### 🧪 Test Ecosystem & Verification

#### **New Safety Verification Tests**
Added regression tests for extreme boundary conditions:
- **`Geometry.Bitmap_Coverage_Underflow`**: Ensures mount fails if the allocated Bitmap region is physically too small for the reported volume capacity.
- **`Geometry.QMask_Coverage_Underflow`**: Validates Silicon Cartography (Q-Mask) region bounds against capacity inflation attacks.
- **`Security.Journal_Ptr_OutOfBounds`**: Verifies that the Chronicle Journal Pointer triggers a Panic state if it points outside valid log boundaries.
- **`Liability.Prevention_Of_Catastrophic_Rollback`**: The "Class Action" Test. Ensures the driver prioritizes a high-generation PANIC Superblock over a low-generation CLEAN Superblock, preventing accidental data reversion to ancient states.

#### **Test Infrastructure Fixes**
- **Huge Block Alignment:** Fixed test fixture geometry in `GeometryLogic.HugeBlockSizeCompatibility` to correctly align the Epoch Ring Start LBA when testing 1MB and 256MB block sizes.
- **ZNS Capacity Mocking:** Updated `HardwareProfile.ZnsNativeMirrorSkip` fixture to provide sufficient capacity (10GB) for ZNS Zone simulation, preventing artificial geometry errors during test execution.

---

## [0.1.1] — 2026-01-06
### **Hardware Compatibility & Safety Hotfix**

This release addresses critical stability issues identified in ZNS (Zoned Namespace), Rotational (HDD), and Embedded (Pico) profiles. These fixes prevent panic conditions and potential metadata corruption on non-standard geometry.

### 🛡️ Critical Safety Fixes

#### **ZNS (Zoned Namespaces)**
- **Write Pointer Protection:** The Cardinal Vote logic in `mount` and `unmount` now correctly detects ZNS devices and skips writing to East/West/South mirrors. This prevents Illegal Write Pointer violations in Sequential Zones.
- **Memory Safety:** Clamped memory allocation logic during Root Anchor verification. Previously, ZNS devices with massive Zone Sizes (e.g., 1.7GB) could trigger an OOM panic by attempting to allocate a full block buffer.
- **Format Alignment:** Strict enforcement of Zone Size alignment during `hn4_format` pre-flight checks.

#### **Pico Profile (Embedded/IoT)**
- **Epoch Ring Geometry:** Fixed a critical desync where the runtime hardcoded a 1MB Ring size even for Pico volumes (which use 2 blocks). This prevents the Epoch Advance logic from overwriting the start of the Cortex (D0) region.

### ⚙️ Logic & Performance

#### **HDD (Rotational Media)**
- **Inertial Damper Enabled:** The "Shotgun Read" logic now respects the `HN4_HW_ROTATIONAL` flag. Parallel speculative reads are disabled on HDDs to prevent head thrashing, restoring sequential throughput performance.

#### **Small Volume / USB**
- **Southbridge Logic Alignment:** Fixed a logic mismatch between `format` and `unmount` regarding the South Superblock (Backup #3). Both paths now strictly enforce the "16x Superblock Size" capacity threshold before attempting to access the South mirror, preventing "Stale Mirror" warnings on small thumb drives.

---

## [0.1.0] — 2026-01-05  
### **First Public Implementation Drop — Core Engine Online**

This release publishes the first working build of the **Hydra-Nexus 4
storage engine**. The foundation is now complete and functional — you can
format a volume, mount it, write to it, unmount it, and verify history via
the immutable Chronicle ledger.

### ✨ Features

#### **Hardware Abstraction Layer (HAL)**
- ARM64 + x86-64 support
- Persistent-memory flush paths including:
  - CLWB / CLFLUSHOPT on x86
  - `dc cvap` / fallback gating on ARM64
- Sync + async I/O submission
- Zone-Append simulation for ZNS devices
- Lightweight allocator with corruption detection and poisoning
- Spinlocks + minimal concurrency primitives

#### **Volume Lifecycle**
- Deterministic **format → mount → unmount** flow
- **Cardinal Vote quorum**
  - Multiple superblock mirrors
  - Split-brain detection
  - Self-healing rewrite logic
- Strict write-ordering guarantees during shutdown:
  - Data flush  
  - Epoch advance  
  - Superblock broadcast  
  - Final persistence barrier  

#### **Epoch Ring & Safety**
- Bounded-skew validation
- Replay-window logic
- Read-only fallback on journal inconsistency
- Taint-decay mechanism to prevent permanent error inflation

#### **Chronicle — Immutable Audit Ledger**
- Hash-linked append-only log
- Anti-rollback verification
- Sequence synchronization during mount
- Volume quarantine on integrity failure

#### **Bitmap & Q-Mask Resource Loading**
- Poison-block awareness
- ECC-protected bitmap words
- Graceful degraded-mode behavior in RO mounts

### 🧪 Testability
- Full test harness enabled
- Deterministic HAL timing + PRNG for reproducibility

### 📄 Documentation
- Core architecture docs published
- Remaining modules are being rewritten for clarity and portability
  and will land shortly

### 💡 Current Status
The engine is functional and self-consistent.  
Right now you can:

✔ run tests  
✔ format a device  
✔ mount / unmount safely  
✔ observe audit history via Chronicle  

More components are coming — fast.

---

## Unreleased
Work in progress includes:

- Advanced allocator & write-combining paths  
- Richer Chronicle introspection tooling  
- Performance tuning + benchmarks  
- Extended HAL backends  
- Expanded failure-simulation test coverage  

---

Let’s change the world — carefully, and with fsync 😉