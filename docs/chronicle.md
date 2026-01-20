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

The Chronicle resides in a pre-allocated circular buffer defined in the Superblock (`sb.journal_start` to `sb.lba_horizon_start`). The total capacity of this ring buffer is fixed at format time.

### 2.1 The Atomic Sector Unit
The Chronicle operates strictly on **Physical Sectors** (typically 512b or 4Kn) as defined by the underlying hardware. Every sector represents exactly one log entry. The system relies on the hardware guarantee of atomic sector writes. Partial writes are detected via the **Dynamic Commit Marker**.

### 2.2 Header Structure (64 Bytes)
The header is located at offset `0x00` of the sector. It is packed and strictly Little Endian (LE). The size is optimized for cache alignment and minimal overhead.

```c
typedef struct HN4_PACKED {
    /* IDENTITY & TIME */
    uint64_t    magic;              /* "CHRONICL" (0x4C43494E4F524843) */
    uint64_t    sequence;           /* Strict Monotonic 64-bit ID */
    uint64_t    timestamp;          /* UTC Nanoseconds */

    /* CONTEXT BINDING */
    hn4_addr_t  old_lba;            /* Pre-mutation LBA (e.g. Previous Root) */
    hn4_addr_t  new_lba;            /* Post-mutation LBA (e.g. New Root) */
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
To detect "Torn Writes" (where drive firmware flushes only half a sector during power loss), we do not use a static magic number at the end of the sector. A static marker could be present from a previous usage of the block.

Instead, the last 8 bytes of the sector contain a calculated marker derived from the header's CRC:

```c
/* Located at sector_size - 8 */
Marker = (uint64_t)HeaderCRC ^ 0xCAFEBABE12345678;
```

**Invariant:** A sector is valid **IF AND ONLY IF**:
1. `magic` matches `HN4_CHRONICLE_MAGIC`.
2. `entry_header_crc` matches the computed CRC of the header fields.
3. The `Marker` at the tail matches the value derived from `entry_header_crc`.

---

## 3. Cryptographic & Security Model

### 3.1 Integrity vs. Malice
*   **Scope:** The Chronicle protects against **Bit Rot** (media degradation), **Firmware Bugs** (misdirected writes), **Power Loss** (torn writes), and **Accidental Overwrites** (software bugs).
*   **Limitation:** It uses CRC32C (Castagnoli) for speed and error detection. It is **NOT** resistant to an adversary with direct block-level write access, who could calculate collisions or rewrite the chain. It is an Audit Log, not a Blockchain.

### 3.2 Anti-Replay / Anti-Relocation
Every entry contains `self_lba`. During verification, the kernel asserts:
`Entry.self_lba == Physical_Disk_LBA`

If a valid sector is copied to a different location (Relocation Attack or misdirected write), verification fails immediately because the internal LBA pointer will not match the read address.

### 3.3 Time-Travel Detection (Anti-Rollback)
The Superblock tracks `sb.last_journal_seq`, the highest sequence number observed.
*   **On Mount:** The kernel reads the Log Head (`L`) and verifies the chain.
*   **Check:** If `L.seq < sb.last_journal_seq`, the volume state has reverted to an older snapshot relative to the Superblock (e.g., VM Snapshot restore of only the journal partition).
*   **Action:** Immediate **LOCKDOWN** (Volume Read-Only). This prevents "Split-Brain" history where two divergent timelines exist for the same volume.

### 3.4 Clock Skew & Trust
The `timestamp` field is informative for user-space auditing. System clock jumps or skew do **not** affect chain validation or integrity logic. Integrity relies solely on Sequence Monotonicity and Cryptographic Linkage.

---

## 4. Algorithms & Logic

### 4.1 Append (Write Path)
The append operation follows a strict **ordering barrier** protocol to ensure crash consistency.

1.  **Sanity Check:** Ensure `NextSeq > PrevSeq`.
    *   **Note:** If `PrevSeq == UINT64_MAX`, lock volume (Sequence Exhaustion). This is practically unreachable (requires billions of years of continuous writes at max IOPS).
2.  **Linkage:** Read the sector at `Head-1` (Previous). Calculate its CRC32C. This becomes `Entry.prev_sector_crc` for the new entry. This creates the hash chain.
3.  **Construction:** Build the new header in a stack-allocated buffer. Calculate `HeaderCRC` over the header fields.
4.  **Marker:** Derive the Dynamic Commit Marker from `HeaderCRC`. Write it to the buffer tail (offset `SectorSize - 8`).
5.  **Barrier 1:** Execute `atomic_thread_fence(release)` to prevent compiler or CPU reordering of memory operations.
6.  **I/O:** Submit Write to `Head` LBA.
7.  **Barrier 2:** Issue `FLUSH / FUA` (Force Unit Access) to the device. Wait for hardware acknowledgement. This guarantees the log entry is persistent.
8.  **Update SB:** Update in-memory `sb.journal_ptr` (increment head) and `sb.last_journal_seq`.
9.  **Persist SB:** Trigger Superblock write to persist the new head pointer.

### 4.2 Integrity Verify (Mount Path)
On mount, the kernel audits the log to determine trust level and recover state.

1.  **Phantom Detection (Forward Look):**
    *   The Superblock `journal_ptr` might be stale (Crash occurred after Log Write, but before SB Write completed).
    *   We speculatively read the sector at `journal_ptr` (which the SB thinks is empty/next).
    *   If valid data exists at that location, AND it cryptographically links to `journal_ptr - 1` (via `prev_sector_crc` and Sequence `N+1`), we have detected a **Phantom Head**.

2.  **Auto-Healing (The Split-Brain Fix):**
    *   If a Phantom Head is verified, we **trust the log** over the Superblock. The log is the ground truth.
    *   We advance the in-memory pointer to include the Phantom Head.
    *   We repeat the check for the *next* sector (Iterative Loop) to recover as many lost entries as possible.
    *   **Action:** Immediately persist the updated Superblock to disk to sync state. If persistence fails, mount as **Read-Only**.

3.  **Reverse Audit (Chain Verification):**
    *   Walk backwards from the valid Head to Genesis (or End of History/Capacity).
    *   Verify `Entry[N].prev_crc == CRC(Entry[N-1])`.
    *   Verify `Entry[N].sequence == Entry[N-1].sequence + 1`.
    *   Stop gracefully if "Event Horizon" (garbage data/zeroes) is reached. If a mismatch is found before the Event Horizon, report **TAMPERED**.

---

## 5. Failure Models & Triage

The Chronicle uses a strict **Escalation Policy** for errors. Note that "PANIC" here refers to Volume Isolation (Read-Only mode), not a Kernel Halt (BSOD/Panic).

| Anomaly Detected | Classification | Kernel Action |
| :--- | :--- | :--- |
| **Bit Rot / Bad CRC** | Corruption | **VOL_PANIC** (Force RO). Log Telemetry. |
| **Broken Chain (Hash Mismatch)** | Tampering | **VOL_PANIC**. Possible adversarial action or severe media corruption. |
| **Sequence Regression** | Time Travel | **LOCKDOWN**. Volume is immutable. Manual intervention required. |
| **Torn Write (Bad Marker)** | Crash Artifact | Treat sector as Empty/Invalid. Rollback Head to N-1. (Safe state). |
| **Heal Persist Fail** | Hardware Fail | **VOL_PANIC**. Cannot guarantee consistency if SB update fails. |

*   **VOL_PANIC:** Volume is marked Read-Only (RO) in memory and potentially on disk via state flags (`HN4_VOL_PANIC`). The filesystem remains mountable for recovery but accepts no writes.
*   **KERNEL PANIC:** System Halt. Never triggered by Chronicle logic unless internal memory corruption is detected (e.g., heap guard violation).

---

## 6. Telemetry & Observability

The volume context maintains atomic counters for health monitoring.
**Note:** These counters are **runtime-only (ephemeral)**. They reset on mount. They are intended for user-space scraping (e.g., via sysfs/ioctl) or Triage Log dumps on unmount.

*   `vol->health.heal_count`: Number of Phantom Heads recovered (indicates unclean shutdowns / power loss events).
*   `vol->health.crc_failures`: Integrity check failures detected in log or blocks (indicates media rot).
*   `vol->health.barrier_failures`: Failed FUA/Flush commands (indicates dying hardware or controller firmware bugs).
*   `vol->last_log_ts`: Timestamp for rate-limiting console spam during error storms.

---

## 7. Known Sharp Edges

1.  **Principal Hash Truncation:** The 32-bit `principal_hash32` is a hint for debugging/audit (e.g., hash of UserID). It is not a cryptographic security control due to collision probability.
2.  **Compiler Reordering:** We rely on C11 `stdatomic` fences. On pre-C11 compilers without GCC-style asm barriers, execution ordering is not guaranteed, potentially compromising crash consistency.
3.  **Wrapping:** When the journal ring buffer wraps, old history is overwritten. Integrity verification is only guaranteed for the active window (current set of valid sectors).

---

## 8. Engineer's Contract

> "The Chronicle is the source of truth. If the Superblock disagrees with the Chronicle, the Superblock is wrong. If the Chronicle disagrees with itself (Hash Chain Break), the Volume is dead."

***
*Confidential - Internal Documentation*