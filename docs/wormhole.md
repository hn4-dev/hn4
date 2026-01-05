# HN4 WORMHOLE PROTOCOL
### *Identity Entanglement & Spatial Overlays*
**Specification:** v5.5 | **Status:** Production Ready | **Module:** `hn4_format.c`

---

## 1. Executive Summary

In traditional file systems (NTFS, ext4, XFS), **Identity is Physical**. A Volume UUID is generated randomly at formatting time to guarantee uniqueness. If an Operating System detects two volumes with the same UUID attached simultaneously, it triggers a "Collision Panic" (forcing a rename, disabling a drive, or crashing).

**HN4 asserts that Identity is Mathematical.**

The **Wormhole Protocol** is a mechanism within the `hn4_format` routine that allows an engineer to deliberately clone the **Identity (UUID)** and **Equation of State** of an existing volume onto new physical media. This creates two distinct physical objects that share the same mathematical topology.

This enables **Stateless Synchronization**, **Zero-Latency RAM Overlays**, and **Temporal Forking** without the overhead of virtualization layers.

---

## 2. The Architecture of Entanglement

The Wormhole is not a "link" or a "shortcut." It is a complete instantiation of the filesystem universe with specific initial conditions.

### 2.1 The Genesis Injection
In `hn4_format.c`, the protocol exposes a bypass to the standard PRNG (Pseudo-Random Number Generator).

```c
typedef struct {
    /* ... standard params ... */
    
    // WORMHOLE CONFIGURATION
    bool        clone_uuid;         // Force deterministic Identity
    hn4_u128_t  specific_uuid;      // The Target Identity to mimic
    uint64_t    mount_intent_flags; // HN4_MNT_WORMHOLE | HN4_MNT_OVERLAY
    uint32_t    root_perms_or;      // Permission Injection (e.g. Force Read-Only)
    hn4_size_t  override_capacity_bytes; // Virtual geometry overlay
} hn4_format_params_t;
```

### 2.2 Iso-Morphic Trajectories
Because the HN4 "Ballistic Engine" calculates data placement based on the Volume UUID and the File Seed ID:
$$ LBA = f(\text{UUID}_{vol}, \text{Seed}_{file}, N) $$

Two volumes formatted as Wormholes (sharing `UUID_vol`) share the **exact same physical lattice**.
*   If you write File A to `Volume_X` (Wormhole 1)...
*   And you write File A to `Volume_Y` (Wormhole 2)...
*   **The data lands on the exact same LBA sector on both drives.**

This guarantees **Binary Isomorphism** without a synchronization table.

---

## 3. Engineering Use Cases

### 3.1 The "Ram-Drive Overlay" (The Accelerator)
*Scenario:* You have a 10TB HDD (Archive). You want NVMe-like speeds for the "Hot" data, but you don't want to manage a caching driver.

*   **Action:** Format a 64GB RAM Disk as a **Wormhole** of the 10TB HDD.
*   **Result:** The OS sees two volumes with the `SAME_UUID`.
*   **The HN4 Driver:** Detects the `HN4_MNT_WORMHOLE` intent flag on the RAM disk. It automatically treats the RAM disk as a **L1 Physical Cache**.
*   **Behavior:**
    *   Reads check the RAM Wormhole first.
    *   Writes go to the RAM Wormhole first (Low Latency), then flush to the HDD (Persistence).
    *   *No complex mapping table is required because the LBAs are mathematically identical.*

### 3.2 Stateless Teleportation (Cluster Sync)
*Scenario:* A distributed cluster needs to sync petabytes of data between Nodes. Using `rsync` requires scanning millions of file headers (slow).

*   **Action:** Format drives on Node A and Node B as Wormholes (Same UUID).
*   **The Sync:** Since the LBA mapping is deterministic, Node A does not need to send filenames or metadata to Node B.
    *   Node A simply streams **Raw Physical Blocks** that have changed.
    *   Node B writes them directly to disk at the offset received.
    *   **Result:** The file system reconstructs itself on Node B automatically. The "files" appear out of the void because the math is valid.

### 3.3 Temporal Forking (The "What If" Sandbox)
*Scenario:* You have a critical production database. You want to test a dangerous schema migration without touching the backup.

*   **Action:**
    1.  Take a Snapshot of Production.
    2.  Format a new volume (or sparse file) as a Wormhole of that Snapshot.
    3.  Inject `root_perms_or = HN4_PERM_WRITE` (Even if the snapshot was Read-Only).
*   **Result:** You now have a writable clone of history.
    *   You can mutate the data on the Wormhole drive.
    *   The original Production drive is mathematically invisible to these changes.
    *   *Note:* The `sb.copy_generation` will diverge, creating a parallel timeline.

---

## 4. Implementation Guide

### 4.1 Invoking a Wormhole (C Code)
To format a drive (`/dev/nvme1n1`) as a Wormhole of an existing UUID:

```c
hn4_format_params_t params = {0};
params.target_profile = HN4_PROFILE_AI;
params.label = "Production_Clone_A";

// ENABLE WORMHOLE PROTOCOL
params.clone_uuid = true;
params.specific_uuid = target_production_uuid; // 128-bit UUID from Source

// Set Intent: This tells the OS "I am a clone, do not panic"
params.mount_intent_flags = HN4_MNT_WORMHOLE | HN4_MNT_VIRTUAL;

// Execute Format
hn4_result_t res = hn4_format(device_hal, &params);
```

### 4.2 The Safety Interlock
To prevent "Split-Brain" corruption (where the OS confuses the two drives and writes metadata to one but data to the other):

1.  **The Intent Check:** The HN4 Mount logic checks `sb.mount_intent`.
2.  **Logic:**
    *   If `Intent == DEFAULT` and a duplicate UUID exists: **PANIC/REJECT**.
    *   If `Intent == WORMHOLE`: **ACCEPT**. The driver enters "Entangled Mode" (Overlay or Independent depending on OS config).

---

## 5. Security Implications

*   **The Sovereign Key:** Since permissions are embedded in the Anchor (on disk), cloning the UUID does **not** bypass encryption or Access Control Lists (Tethers).
*   **Audit Trail:** Creation of a Wormhole is an event that generates a distinct **Chronicle Entry** in the audit log, ensuring that administrators can trace the lineage of the forked data.

---

**Summary:** The Wormhole Protocol transforms storage from a "Container of Files" into a **"Mathematical Function of State."** It allows data to exist in superposition across multiple physical mediums simultaneously.