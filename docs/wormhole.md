# HN4 WORMHOLE PROTOCOL
### *Deterministic Identity Cloning & Geometric Overlays*

**Status:** Implementation Standard v5.5
**Module:** `hn4_format.c` / `hn4_mount.c`
**Metric:** Address Space Isomorphism

---

## 1. Operational Theory

In standard filesystem architectures (ext4, XFS), a Volume UUID is a random 128-bit integer generated at format time to ensure global uniqueness. Operating System kernels rely on this uniqueness to multiplex block devices; detecting two mounted volumes with identical UUIDs typically triggers a collision error or kernel panic to prevent data corruption.

**HN4 decouples Identity from Physical Instantiation.**

The **Wormhole Protocol** is a format-time directive that forces a new volume to inherit the specific **UUID**, **Generation Count**, and **Geometry Parameters** of an existing volume.

Because HN4 utilizes algebraic addressing (where physical placement is a function of the Volume UUID and File Seed), two volumes formatted with identical parameters possess **Binary Isomorphism**. Writing a specific logical block to File $A$ will calculate the exact same physical LBA on both drives, regardless of their underlying hardware differences.

This enables stateless synchronization, hierarchy-free RAM caching, and temporal snapshotting without virtualization overhead.

---

## 2. Implementation Mechanics

The protocol is invoked during the `hn4_format` routine. Instead of seeding the Pseudo-Random Number Generator (PRNG) for a new identity, the caller injects a specific state vector.

### 2.1 The Format Injection
The `hn4_format_params_t` structure (in `hn4.h`) exposes specific fields to bypass standard initialization:

```c
typedef struct {
    // ... standard geometry params ...

    /* WORMHOLE CONTROL */
    bool        clone_uuid;              // Disable PRNG for Identity
    hn4_u128_t  specific_uuid;           // Target UUID to replicate
    
    /* OVERLAY CONFIGURATION */
    uint64_t    mount_intent_flags;      // Flags: HN4_MNT_WORMHOLE | HN4_MNT_VIRTUAL
    hn4_size_t  override_capacity_bytes; // Force logical geometry (Virtual Size)
    
    /* SECURITY OVERRIDES */
    uint32_t    root_perms_or;           // Inject permission masks (e.g., Force RO)
} hn4_format_params_t;
```

### 2.2 Address Isomorphism
The core addressing equation depends on the volume identity:
$$ LBA = f(\text{UUID}_{vol}, \text{Seed}_{file}, \text{Block}_{index}) $$

If $\text{UUID}_{vol}$ is identical between Drive A and Drive B, the function $f$ returns identical coordinates for any given file.
*   **Result:** Data locality is deterministic across physical boundaries. No lookup table or translation layer is required to map data between the two volumes.

---

## 3. Engineering Use Cases

### 3.1 Hardware-Accelerated Overlays (L1 Physical Cache)
**Problem:** Accelerating a slow, large capacity tier (HDD/Tape) with a small, fast tier (RAM/Optane) usually requires a block-level caching driver (e.g., dm-cache, bcache).

**Solution:** Format a RAM block device as a Wormhole of the HDD.
1.  **Format:** Use `override_capacity_bytes` to make the 64GB RAM drive report the same logical capacity (e.g., 10TB) as the HDD source.
2.  **Mount:** Mount the RAM drive with `HN4_MNT_WORMHOLE`.
3.  **Behavior:** The HN4 driver treats the RAM drive as a sparse physical overlay.
    *   **Writes:** Go to the RAM Wormhole (Low Latency).
    *   **Flush:** Dirty blocks from RAM are written to the *exact same LBA* on the HDD. No translation map is needed because the LBAs are mathematically identical.

### 3.2 Stateless Delta Synchronization
**Problem:** syncing petabyte-scale datasets between nodes usually requires traversing directory trees (rsync) to identify changes.

**Solution:** If Node A and Node B share Wormhole volumes (Same UUID):
1.  **Logic:** The physical layout is identical.
2.  **Sync:** Node A streams a log of changed physical sectors (LBA + Length).
3.  **Apply:** Node B writes these sectors directly to disk.
4.  **Result:** The filesystem structure on Node B updates automatically. New files "appear" and deleted files vanish without Node B ever processing a metadata transaction, because the underlying mathematical structure was synchronized at the block level.

### 3.3 Temporal Forking (Sandbox Environments)
**Problem:** Testing schema migrations on production data requires costly backup/restore cycles.

**Solution:**
1.  **Snapshot:** Create a read-only snapshot of the Production Volume.
2.  **Fork:** Format a new sparse volume as a Wormhole of that snapshot.
3.  **Diverge:** Mount the Wormhole as Read-Write (`root_perms_or = HN4_PERM_WRITE`).
4.  **Result:** The Wormhole shares the history of the production volume up to the fork point. Writes to the Wormhole diverge the `copy_generation`, creating a parallel timeline. The original data remains immutable; the new volume accumulates the delta.

---

## 4. Safety Interlocks

Allowing duplicate UUIDs is dangerous for standard OS kernel logic. HN4 implements specific gates to prevent "Split-Brain" corruption.

### 4.1 The Intent Flag (`mount_intent`)
The Superblock contains a `mount_intent` field.
*   **Standard Volume:** `Intent == 0`. The driver asserts exclusive ownership of the UUID. If a duplicate is found, the mount fails (Safety Panic).
*   **Wormhole Volume:** `Intent == HN4_MNT_WORMHOLE`. The driver registers the volume as a **Satellite**. It acknowledges the UUID conflict is intentional and suppresses the kernel panic.

### 4.2 Strict Flush Requirement
Wormhole volumes operating as overlays often act as write-back caches.
*   **Constraint:** The underlying HAL must support `HN4_HW_STRICT_FLUSH`.
*   **Reasoning:** If the overlay (RAM) acknowledges a write but fails to flush to the backing store (HDD) before a crash, the mathematical consistency between the two timelines is broken.

---

## 5. Security Context

Cloning a UUID does not bypass Data-at-Rest Encryption (DARE).

*   **Key Independence:** The Volume UUID is a cleartext identifier. The Master Key used to decrypt payload data is wrapped in the **Cortex**.
*   **Behavior:** If an attacker creates a Wormhole of an encrypted drive, they replicate the encrypted ciphertext. Without the specific cryptographic key for that volume instance, the Wormhole contains only high-entropy noise.
*   **Audit:** Wormhole creation is a logged event. The `hn4_format` routine writes a genesis entry to the Chronicle (Audit Log), cryptographically chaining the fork event to the parent volume's history.

---

## 6. Summary

The Wormhole Protocol treats storage not as a container, but as a function. By injecting initial conditions, we force distinct physical substrates to evaluate the same storage equation, enabling seamless data mobility and layering without the performance cost of virtualization tables.