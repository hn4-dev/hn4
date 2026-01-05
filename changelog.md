# Hydra-Nexus 4 — Changelog

All notable changes to **HN4 (Hydra-Nexus 4)** will be documented in this file.

This project follows a stability-first philosophy: correctness, durability,
and observability take priority over features. Entries below describe
externally-visible behavior, storage-format semantics, and major internal
architecture shifts.

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