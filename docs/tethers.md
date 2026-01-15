# HN4 TETHER PROTOCOL
### *Secure Remote Attestation & Access Leasing*

**Status:** Implementation Standard v9.3
**Module:** `hn4_tether.c` (Conceptual) / Integrated via `hn4_anchor.t`
**Metric:** Zero-Knowledge Access Control

---

## 1. Executive Summary

Traditional Access Control Lists (ACLs) are static. Permissions are stored with the file metadata (chmod/chown) and enforced by the local kernel.

**The Tether Protocol** decouples authorization from the filesystem.

A **Tether** is a cryptographically signed lease structure that grants temporary access rights to a specific object (Anchor) without requiring the user to possess the Sovereign Key. It functions like a "Bearer Token" or a pre-signed URL for block storage.

This enables:
1.  **Time-Bounded Access:** "Read-Only access to File X for 5 minutes."
2.  **Remote Attestation:** A central authority can sign a Tether for a client without modifying the disk metadata.
3.  **Zero-Knowledge Sharing:** The filesystem driver validates the signature without needing to know *who* the user is, only that they hold a valid token.

---

## 2. On-Disk Structure

The Tether is defined in `hn4.h` as `hn4_tether_t`. It is designed to fit within a single 128-byte cache line for atomic loading.

```c
typedef struct HN4_PACKED {
    uint32_t    target_type;        /* 0x00: Resource Type (File/Dir) */
    uint32_t    permissions;        /* 0x04: Access Mask (Read/Write) */
    uint64_t    expiry_ts;          /* 0x08: Expiration (Nanoseconds) */
    hn4_u128_t  target_value;       /* 0x10: Target Anchor Seed ID */
    uint8_t     signature[64];      /* 0x20: Ed25519 Signature */
    uint8_t     padding[32];        /* 0x60: Reserved (Zeroed) */
} hn4_tether_t;
```

### 2.1 Field Semantics

*   **target_type:** Defines the scope.
    *   `0`: Specific Anchor (File).
    *   `1`: Namespace (Directory/Tag Group).
    *   `2`: Volume (Whole Disk).
*   **permissions:** A subset of the standard HN4 permission bitmask (`HN4_PERM_READ`, `HN4_PERM_WRITE`).
*   **expiry_ts:** Absolute system time (UTC Nanoseconds). If `Current_Time > Expiry`, the Tether is invalid.
*   **target_value:** The 128-bit immutable `seed_id` of the object being accessed.
*   **signature:** A cryptographic signature covering bytes `0x00` to `0x1F` (the first 32 bytes).

---

## 3. The Validation Pipeline

When a user presents a Tether to the `hn4_open` or `hn4_mount` API, the kernel performs a strictly ordered validation sequence.

### 3.1 Step 1: Clock Check
The driver compares `tether.expiry_ts` against the hardware monotonic clock (`hn4_hal_get_time_ns()`).
*   **Logic:** `if (Now > Tether.Expiry) return HN4_ERR_TETHER_EXPIRED;`
*   **Constraint:** This requires the host system clock to be synchronized. In embedded systems without RTC, Tethers may be rejected or require a nonce-based extension (outside v1 spec).

### 3.2 Step 2: Scope Check
The driver compares `tether.target_value` against the object being requested.
*   **Logic:** If the user is trying to open File A, but the Tether is signed for File B, access is denied. This prevents "Token Replay" attacks against different files.

### 3.3 Step 3: Cryptographic Verification
The driver verifies the Ed25519 signature using the **Volume Public Key** (stored in the Superblock or loaded at mount time).

*   **Message:** The first 32 bytes of the Tether struct (Type, Perms, Expiry, Target).
*   **Signature:** The 64-byte `signature` field.
*   **Public Key:** The volume owner's key.

**If the signature matches, the driver grants the access defined in `tether.permissions`.**

---

## 4. Operational Workflows

### 4.1 Scenario: The AI Training Job
**Context:** A central server hosts a 10TB dataset. A worker node needs access for 1 hour to train a model.

1.  **Request:** Worker requests access from the Auth Server.
2.  **Minting:** Auth Server generates a Tether:
    *   `Target`: Dataset_UUID
    *   `Perms`: READ_ONLY
    *   `Expiry`: Now + 1 Hour
    *   **Sign:** Auth Server signs it with the Volume Private Key.
3.  **Transfer:** The 128-byte Tether is sent to the Worker.
4.  **Access:** The Worker passes the Tether to the HN4 driver on the storage node.
5.  **Execution:** The driver validates the signature. It allows the Worker to read data until the timestamp expires.
6.  **Revocation:** Once the time elapses, the driver automatically rejects subsequent read requests. No network call is needed to revoke access.

### 4.2 Scenario: The Immutable Snapshot
**Context:** A ransomware-proof backup.

1.  **Minting:** The backup software creates a Tether with `PERM_WRITE` for the duration of the backup window (e.g., 2am - 4am).
2.  **Constraint:** Outside this window, no Tether exists.
3.  **Defense:** Even if the backup server is compromised at 5am, it cannot delete previous backups because it cannot generate a valid Tether (it doesn't hold the Volume Private Key, only a temporary lease).

---

## 5. Security Analysis

### 5.1 Replay Defense
*   **Attack:** An attacker intercepts a valid Tether and tries to use it later.
*   **Defense:** The `expiry_ts` limits the window of opportunity. Once expired, the Tether is useless mathematical garbage.

### 5.2 Forgery Defense
*   **Attack:** An attacker modifies the `permissions` bitmask from Read to Write.
*   **Defense:** The signature covers the permission bits. Modifying any bit in the first 32 bytes invalidates the Ed25519 signature.

### 5.3 Elevation of Privilege
*   **Attack:** Using a Tether for File A to access File B.
*   **Defense:** The `target_value` (File ID) is signed. The driver checks that the Tether matches the object being accessed.

---

## 6. Integration with ZNS and NVM

The Tether Protocol is entirely logical; it does not touch the disk format.
*   **ZNS:** Tethers can authorize `ZONE_APPEND` operations without granting `ZONE_RESET` privileges.
*   **NVM:** In DAX mode, the kernel can map a file into userspace memory *only* if a valid Tether is presented. The kernel revocation mechanism (e.g., shooting down PTEs) enforces expiration.

---

## 7. Summary

The Tether Protocol transforms access control from a static "List" into a dynamic "Capability." It allows distributed systems to share storage resources securely without shared authentication databases, relying purely on cryptography and time.