# HN4 STORAGE ENGINE: CHRONICLE SUBSYSTEM
**File:** `hn4_chronicle.md`  
**Version:** 8.7 (Refined / Production)  
**Status:** **RATIFIED**  
**Author:** Core Storage Infrastructure Team  
**System:** Hydra-Nexus 4 (HN4) Kernel Module  

---

## 1. Executive Summary

The **Chronicle** is the immutable, circular audit log and journaling subsystem for the HN4 file system. It provides a cryptographically linked "Chain of Custody" for all metadata mutations. Unlike traditional Write-Ahead Logs (WAL) which exist primarily for crash recovery, the Chronicle is designed for **Forensic Integrity**.

It guarantees:
1.  **Atomicity:** Metadata operations are either fully committed or fully discarded.
2.  **Linearity:** History cannot branch. Sequence numbers are strictly monotonic.
3.  **Tamper-Evidence:** Chains are linked via CRC32C and anti-replay LBA bindings.
4.  **Anti-Tearing:** Sector writes use a Dynamic Commit Marker derived from header content.

---

## 2. On-Disk Layout & ABI

The Chronicle resides in a pre-allocated circular buffer defined in the Superblock (`sb.journal_start` to `sb.lba_horizon_start`).

### 2.1 The Atomic Sector Unit
The Chronicle operates strictly on **Physical Sectors** (typically 512b or 4Kn). Every sector represents exactly one log entry. Partial writes are detected via the **Dynamic Commit Marker**.

### 2.2 Header Structure (64 Bytes)
The header is located at offset `0x00` of the sector. It is packed and strictly Little Endian (LE).

```c
typedef struct HN4_PACKED {
    /* IDENTITY & TIME */
    uint64_t    magic;              /* "CHRONICL" (0x4C43494E4F524843) */
    uint64_t    sequence;           /* Strict Monotonic 64-bit ID */
    uint64_t    timestamp;          /* UTC Nanoseconds */

    /* CONTEXT BINDING */
    hn4_addr_t  old_lba;            /* Pre-mutation LBA */
    hn4_addr_t  new_lba;            /* Post-mutation LBA */
    hn4_addr_t  self_lba;           /* ANTI-REPLAY: Must match physical location */

    /* METADATA */
    uint32_t    principal_hash32;   /* Truncated Principal ID (Hint only) */
    uint16_t    version;            /* Format Version (v4) */
    uint16_t    op_code;            /* Operation Type (COMMIT, ROLLBACK, etc) */

    /* CRYPTOGRAPHIC LINKAGE */
    uint32_t    prev_sector_crc;    /* CRC32C of the N-1 Sector */
    uint32_t    entry_header_crc;   /* CRC32C of bytes 0x00 to 0x3B */
} hn4_chronicle_header_t;
```

### 2.3 The Dynamic Commit Marker
To detect "Torn Writes" (where drive firmware flushes only half a sector during power loss), we do not use a static magic number at the end of the sector.

Instead, the last 8 bytes of the sector contain a calculated marker:
```c
Marker = (uint64_t)HeaderCRC ^ 0xCAFEBABE12345678;
```
**Invariant:** A sector is valid **IF AND ONLY IF** `HeaderCRC` matches the header data **AND** the `Marker` at the tail matches the derived value.

---

## 3. Cryptographic & Security Model

### 3.1 Integrity vs. Malice
*   **Scope:** The Chronicle protects against **Bit Rot**, **Firmware Bugs**, **Power Loss**, and **Accidental Overwrites**.
*   **Limitation:** It uses CRC32C. It is **NOT** resistant to an adversary with direct block-level write access, who could calculate collisions.

### 3.2 Anti-Replay / Anti-Relocation
Every entry contains `self_lba`. During verification, the kernel asserts:
`Entry.self_lba == Physical_Disk_LBA`

If a valid sector is copied to a different location (Relocation Attack), verification fails immediately.

### 3.3 Time-Travel Detection (Anti-Rollback)
The Superblock tracks `sb.last_journal_seq`.
*   **On Mount:** The kernel reads the Log Head (`L`).
*   **Check:** If `L.seq < sb.last_journal_seq`, the volume has been rolled back (e.g., VM Snapshot restore).
*   **Action:** Immediate **LOCKDOWN** (Volume Read-Only).

### 3.4 Clock Skew & Trust
The `timestamp` field is informative. System clock jumps or skew do **not** affect chain validation or integrity logic. Integrity relies solely on Sequence Monotonicity and Cryptographic Linkage.

---

## 4. Algorithms & Logic

### 4.1 Append (Write Path)
The append operation follows a strict **ordering barrier** to ensure crash consistency.

1.  **Sanity Check:** Ensure `NextSeq > PrevSeq`.
    *   **Note:** If `PrevSeq == UINT64_MAX`, lock volume. This is practically unreachable (requires billions of years of continuous writes).
2.  **Linkage:** Read `Head-1`. Calculate its CRC. This becomes `Entry.prev_sector_crc`.
3.  **Construction:** Build header in stack buffer. Calculate `HeaderCRC`.
4.  **Marker:** Derive Commit Marker. Write to buffer tail.
5.  **Barrier 1:** `atomic_thread_fence(release)` (Prevent CPU reordering).
6.  **I/O:** Submit Write `Head`.
7.  **Barrier 2:** Issue `FLUSH / FUA`. Wait for hardware ack.
8.  **Update SB:** Update `sb.journal_ptr` and `sb.last_journal_seq`.
9.  **Persist SB:** Write Superblock to disk.

### 4.2 Integrity Verify (Mount Path)
On mount, the kernel audits the log to determine trust level.

1.  **Phantom Detection (Forward Look):**
    *   The Superblock `journal_ptr` might be stale (Crash after Log Write, before SB Write).
    *   We read `journal_ptr` (expected empty).
    *   If valid data exists and cryptographically links to `journal_ptr - 1`, we detect a **Phantom Head**.

2.  **Auto-Healing (The Split-Brain Fix):**
    *   If a Phantom Head is verified, we **trust the log**.
    *   We advance the in-memory pointer.
    *   We verify the next sector (Iterative Loop).
    *   **Action:** Immediately persist the updated Superblock. If persistence fails, mount as **Read-Only**.

3.  **Reverse Audit:**
    *   Walk backwards from Head to Genesis (or End of History).
    *   Verify `Entry[N].prev_crc == CRC(Entry[N-1])`.
    *   Stop gracefully if "Event Horizon" (garbage data) is reached.

---

## 5. Failure Models & Triage

The Chronicle uses a strict **Escalation Policy** for errors. Note that "PANIC" here refers to Volume Isolation, not Kernel Halt.

| Anomaly Detected | Classification | Kernel Action |
| :--- | :--- | :--- |
| **Bit Rot / Bad CRC** | Corruption | **VOL_PANIC** (Force RO). Log Telemetry. |
| **Broken Chain (Hash Mismatch)** | Tampering | **VOL_PANIC**. Possible adversarial action. |
| **Sequence Regression** | Time Travel | **LOCKDOWN**. Volume is immutable. |
| **Torn Write (Bad Marker)** | Crash Artifact | Treat as Empty. Rollback to N-1. |
| **Heal Persist Fail** | Hardware Fail | **VOL_PANIC**. Cannot guarantee consistency. |

*   **VOL_PANIC:** Volume is marked Read-Only (RO) in memory and potentially on disk via state flags. System continues running.
*   **KERNEL PANIC:** System Halt. Never triggered by Chronicle unless memory corruption detected (e.g., heap guards).

---

## 6. Telemetry & Observability

The volume context maintains atomic counters for health monitoring.
**Note:** These counters are **runtime-only (ephemeral)**. They reset on mount. They are intended for user-space scraping (e.g., via sysfs/ioctl) or Triage Log dumps on unmount.

*   `vol->stats.heal_count`: Number of Phantom Heads recovered (indicates unclean shutdowns).
*   `vol->stats.crc_failures`: Integrity check failures (indicates media rot).
*   `vol->stats.barrier_failures`: Failed FUA/Flush commands (indicates dying hardware).
*   `vol->last_log_ts`: Timestamp for rate-limiting console spam during storms.

---

## 7. Known Sharp Edges

1.  **Principal Hash Truncation:** The 32-bit Principal ID is a hint. It is not a security control. Collisions are expected at scale.
2.  **Compiler Reordering:** We rely on C11 `stdatomic` fences. On pre-C11 compilers without GCC-style asm barriers, ordering is not guaranteed.
3.  **Wrapping:** When the journal wraps, old history is overwritten. Integrity is only guaranteed for the active window.

---

## 8. Engineer's Contract

> "The Chronicle is the source of truth. If the Superblock disagrees with the Chronicle, the Superblock is wrong. If the Chronicle disagrees with itself (Hash Chain Break), the Volume is dead."

***
*Confidential - Internal Use Only*