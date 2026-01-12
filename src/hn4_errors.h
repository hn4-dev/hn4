/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4_errors.h
 * STATUS:      RATIFIED (v5.5)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 *   Defines the status codes for the HN4 Storage Engine.
 *   
 * ERROR PARADIGM: THE NEGATIVE MANIFOLD
 *   - 0      : The Singularity (Success)
 *   - > 0    : The Positive Manifold (Informational / Non-Fatal)
 *   - < 0    : The Negative Manifold (Hard Errors / Fatal)
 */

#ifndef HN4_ERRORS_H
#define HN4_ERRORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type definition for results (Signed 32-bit Integer) */
typedef int32_t hn4_result_t;

/* =========================================================================
 * 0. THE SINGULARITY (SUCCESS)
 * ========================================================================= */
#define HN4_OK                          0

/* 
 * POSITIVE MANIFOLD (Informational)
 * Used when an operation succeeded, but with specific conditions.
 * NOTE: HN4_IS_OK() returns true for these values.
 */
#define HN4_INFO_PENDING                1   /* Async Operation Queued / In-Flight */
#define HN4_INFO_HEALED                 2   /* Read success, but ECC/Helix Repair was triggered (Data was corrected) */
#define HN4_INFO_SPARSE                 3   /* Read success, returned implicit Zeros (Holo-Lattice/Hole) */
#define HN4_INFO_HORIZON_FALLBACK       4   /* Allocation succeeded, but forced to Linear Log (D1.5) instead of Flux (D1) */
#define HN4_INFO_THAWED                 5   /* Write succeeded, but data was forced to decompress due to entropy */

/* 
 * Helper Macros 
 * WARNING: HN4_IS_OK(x) is true for Positive Manifold codes (1..5).
 * If you require strict success (0), check (x == HN4_OK).
 */
#define HN4_IS_OK(x)    ((x) >= 0)
#define HN4_IS_ERR(x)   ((x) < 0)


/* =========================================================================
 * 1. THE VOID (SPACE & ALLOCATION)
 * Range: [-0x100 to -0x1FF]
 * ========================================================================= */
/* Physical storage exhausted. No trajectories available. */
#define HN4_ERR_ENOSPC                  -0x100

/* D1 (Flux) is full, and D1.5 (Horizon) is also full. */
#define HN4_ERR_EVENT_HORIZON           -0x101

/* The Void Engine hit max k=12 collision limit (Hash Saturation). */
#define HN4_ERR_GRAVITY_COLLAPSE        -0x102

/* The Armored Bitmap in RAM failed ECC check during allocation. */
#define HN4_ERR_BITMAP_CORRUPT          -0x103

/* Allocation requested specific Fractal alignment but failed. */
#define HN4_ERR_ALIGNMENT_FAIL          -0x104

/* Hardware atomic operation (CAS) timed out under contention. */
#define HN4_ERR_ATOMICS_TIMEOUT         -0x105

/* ZNS Zone Append failed (Zone Full, Read-Only, or Offline). */
#define HN4_ERR_ZONE_FULL               -0x106

/* Volume is marked VOL_PENDING_WIPE. No allocations allowed. */
#define HN4_ERR_WIPE_PENDING            -0x107


/* =========================================================================
 * 2. THE CORTEX (IDENTITY & LOOKUP)
 * Range: [-0x200 to -0x2FF]
 * ========================================================================= */
/* Anchor not found in D0 or Nano-Cortex Cache (ENOENT). */
#define HN4_ERR_NOT_FOUND               -0x200

/* Anchor exists but is marked HN4_FLAG_TOMBSTONE (Deleted). */
#define HN4_ERR_TOMBSTONE               -0x201

/* Anchor ID mismatch (Seed ID vs Payload). Ghost entry detected. */
#define HN4_ERR_ID_MISMATCH             -0x202

/* Tag Query returned too many results for the provided buffer. */
#define HN4_ERR_TAG_OVERFLOW            -0x203

/* Name too long for Inline Buffer and Extension Chain is full. */
#define HN4_ERR_NAME_TOO_LONG           -0x204


/* =========================================================================
 * 3. THE SOVEREIGN (SECURITY & PERMISSION)
 * Range: [-0x300 to -0x3FF]
 * ========================================================================= */
/* General Access Denied (No Key, No Tether). */
#define HN4_ERR_ACCESS_DENIED           -0x300

/* Operation rejected by PERM_IMMUTABLE flag (WORM violation). */
#define HN4_ERR_IMMUTABLE               -0x301

/* Ed25519 Signature verification failed. */
#define HN4_ERR_SIG_INVALID             -0x302

/* Tether has expired (Current Time > Tether.expiry_ts). */
#define HN4_ERR_TETHER_EXPIRED          -0x303

/* Operation requires Sovereign Key (Root) privileges. */
#define HN4_ERR_NOT_SOVEREIGN           -0x304

/* Volume is in VOL_LOCKED state (Ransomware protection/Lockdown). */
#define HN4_ERR_VOLUME_LOCKED           -0x305

/* Audit Chronicle Write Failed (Strict auditing enforces op failure). */
#define HN4_ERR_AUDIT_FAILURE           -0x306


/* =========================================================================
 * 4. THE HELIX (INTEGRITY & HARDWARE)
 * Range: [-0x400 to -0x4FF]
 * ========================================================================= */
/* Generic Hardware IO Error (EIO). */
#define HN4_ERR_HW_IO                   -0x400

/* Data CRC32C mismatch. Auto-Medic failed to heal. */
#define HN4_ERR_DATA_ROT                -0x401

/* Reed-Solomon reconstruction failed (Too many bad shards). */
#define HN4_ERR_PARITY_BROKEN           -0x402

/* Block Header well_id mismatch (Phantom Read / Stale Data). */
#define HN4_ERR_PHANTOM_BLOCK           -0x403

/* Decompression (LZ4/ZSTD) failed (Corrupt Payload). */
#define HN4_ERR_DECOMPRESS_FAIL         -0x404

/* Drive Temperature critical. Operation throttled/aborted. */
#define HN4_ERR_THERMAL_CRITICAL        -0x405

/* CPU Integrity Check failed (RAM/ALU unstable). */
#define HN4_ERR_CPU_INSANITY            -0x406

/* Volume state is VOL_TOXIC. Media is dying. */
#define HN4_ERR_MEDIA_TOXIC             -0x407

/* Encrypted block MAC verification failed. */
#define HN4_ERR_ENCRYPTED_ROT           -0x408

/* Block Header CRC mismatch. */
#define HN4_ERR_HEADER_ROT              -0x450

/* Block Payload CRC mismatch (Specific). */
#define HN4_ERR_PAYLOAD_ROT             -0x451


/* =========================================================================
 * 5. THE CHRONO-SPHERE (TIME & STATE)
 * Range: [-0x500 to -0x5FF]
 * ========================================================================= */
/* Generation Counter mismatch (Phantom Write Defense). */
#define HN4_ERR_GENERATION_SKEW         -0x500

/* Requested Epoch ID not found in the Ring (Too old). */
#define HN4_ERR_EPOCH_LOST              -0x501

/* Snapshot ID not found or invalid. */
#define HN4_ERR_SNAPSHOT_INVALID        -0x502

/* Reflink/Dedupe attempted across incompatible boundaries. */
#define HN4_ERR_QUANTUM_VIOLATION       -0x503

/* System Clock is behind Last Mount Time (Clock Skew). */
#define HN4_ERR_TIME_DILATION           -0x504

/* Chronicle indicates Tampering (Sequence ID gap detected). */
#define HN4_ERR_TAMPERED                -0x505

/* Attempted to write to a Read-Only Historical View. */
#define HN4_ERR_TIME_PARADOX            -0x506


/* =========================================================================
 * 6. THE MANIFOLD (SYSTEM & DRIVER)
 * Range: [-0x600 to -0x6FF]
 * ========================================================================= */
/* Superblock Magic Invalid. */
#define HN4_ERR_BAD_SUPERBLOCK          -0x600

/* Feature flag in SB.incompat_flags not supported by driver. */
#define HN4_ERR_VERSION_INCOMPAT        -0x601

/* Not enough Kernel RAM for Nano-Cortex or buffers. */
#define HN4_ERR_NOMEM                   -0x602

/* GPU Direct (Tensor Tunnel) Setup Failed. */
#define HN4_ERR_DMA_MAPPING             -0x603

/* Operation invalid for this Device Profile (e.g. ZNS op on HDD). */
#define HN4_ERR_PROFILE_MISMATCH        -0x604

/* File too large for 32-bit Pico Profile. */
#define HN4_ERR_PICO_LIMIT              -0x605

/* Endianness check failed (sb.endian_tag mismatch). */
#define HN4_ERR_ENDIAN_MISMATCH         -0x606

/* Driver Internal Logic Error (Bug/Assert). */
#define HN4_ERR_INTERNAL_FAULT          -0x607

/* Drive capacity violates Profile Min/Max limits. */
#define HN4_ERR_GEOMETRY                -0x608

/* Invalid argument passed to API. */
#define HN4_ERR_INVALID_ARGUMENT        -0x609

/* Operation requires zeroed/initialized memory/metadata. */
#define HN4_ERR_UNINITIALIZED           -0x60A

/* Object already exists (Collision). */
#define HN4_ERR_EEXIST                  -0x610

/* Compression output larger than input. */
#define HN4_ERR_COMPRESSION_INEFFICIENT -0x611

/* Compression algorithm ID not recognized/supported by driver. */
#define HN4_ERR_ALGO_UNKNOWN            -0x612

/* Cryptographic key expired or revoked. */
#define HN4_ERR_KEY_EXPIRED             -0x613

#define HN4_ERR_INTERNAL                -0x614
/* =========================================================================
 * 7. UTILITY FUNCTIONS
 * ========================================================================= */

/**
 * hn4_strerror()
 * Returns a static string representation of the error code.
 * Useful for Triage Logs (Spec 21.4).
 * 
 * @param res The error code to stringify.
 * @return A const string pointer (do not free).
 */
static inline const char* hn4_strerror(hn4_result_t res) {
    switch (res) {
        /* --- Singularity --- */
        case HN4_OK:                        return "SUCCESS";
        
        /* --- Positive Manifold --- */
        case HN4_INFO_PENDING:              return "PENDING";
        case HN4_INFO_HEALED:               return "HEALED_VIA_HELIX";
        case HN4_INFO_SPARSE:               return "SPARSE_READ";
        case HN4_INFO_HORIZON_FALLBACK:     return "HORIZON_FALLBACK";
        case HN4_INFO_THAWED:               return "THAWED";
        
        /* --- The Void --- */
        case HN4_ERR_ENOSPC:                return "ERR_ENOSPC";
        case HN4_ERR_EVENT_HORIZON:         return "ERR_EVENT_HORIZON";
        case HN4_ERR_GRAVITY_COLLAPSE:      return "ERR_GRAVITY_COLLAPSE";
        case HN4_ERR_BITMAP_CORRUPT:        return "ERR_BITMAP_CORRUPT";
        case HN4_ERR_ALIGNMENT_FAIL:        return "ERR_ALIGNMENT_FAIL";
        case HN4_ERR_ATOMICS_TIMEOUT:       return "ERR_ATOMICS_TIMEOUT";
        case HN4_ERR_ZONE_FULL:             return "ERR_ZONE_FULL";
        case HN4_ERR_WIPE_PENDING:          return "ERR_WIPE_PENDING";
        
        /* --- The Cortex --- */
        case HN4_ERR_NOT_FOUND:             return "ERR_NOT_FOUND";
        case HN4_ERR_TOMBSTONE:             return "ERR_TOMBSTONE";
        case HN4_ERR_ID_MISMATCH:           return "ERR_ID_MISMATCH";
        case HN4_ERR_TAG_OVERFLOW:          return "ERR_TAG_OVERFLOW";
        case HN4_ERR_NAME_TOO_LONG:         return "ERR_NAME_TOO_LONG";
        
        /* --- The Sovereign --- */
        case HN4_ERR_ACCESS_DENIED:         return "ERR_ACCESS_DENIED";
        case HN4_ERR_IMMUTABLE:             return "ERR_IMMUTABLE";
        case HN4_ERR_SIG_INVALID:           return "ERR_SIG_INVALID";
        case HN4_ERR_TETHER_EXPIRED:        return "ERR_TETHER_EXPIRED";
        case HN4_ERR_NOT_SOVEREIGN:         return "ERR_NOT_SOVEREIGN";
        case HN4_ERR_VOLUME_LOCKED:         return "ERR_VOLUME_LOCKED";
        case HN4_ERR_AUDIT_FAILURE:         return "ERR_AUDIT_FAILURE";
        
        /* --- The Helix --- */
        case HN4_ERR_HW_IO:                 return "ERR_HW_IO";
        case HN4_ERR_DATA_ROT:              return "ERR_DATA_ROT";
        case HN4_ERR_HEADER_ROT:            return "ERR_HEADER_ROT";
        case HN4_ERR_PAYLOAD_ROT:           return "ERR_PAYLOAD_ROT";
        case HN4_ERR_ENCRYPTED_ROT:         return "ERR_ENCRYPTED_ROT";
        case HN4_ERR_PARITY_BROKEN:         return "ERR_PARITY_BROKEN";
        case HN4_ERR_PHANTOM_BLOCK:         return "ERR_PHANTOM_BLOCK";
        case HN4_ERR_DECOMPRESS_FAIL:       return "ERR_DECOMPRESS_FAIL";
        case HN4_ERR_THERMAL_CRITICAL:      return "ERR_THERMAL_CRITICAL";
        case HN4_ERR_CPU_INSANITY:          return "ERR_CPU_INSANITY";
        case HN4_ERR_MEDIA_TOXIC:           return "ERR_MEDIA_TOXIC";
        
        /* --- The Chrono-Sphere --- */
        case HN4_ERR_GENERATION_SKEW:       return "ERR_GENERATION_SKEW";
        case HN4_ERR_EPOCH_LOST:            return "ERR_EPOCH_LOST";
        case HN4_ERR_SNAPSHOT_INVALID:      return "ERR_SNAPSHOT_INVALID";
        case HN4_ERR_QUANTUM_VIOLATION:     return "ERR_QUANTUM_VIOLATION";
        case HN4_ERR_TIME_DILATION:         return "ERR_TIME_DILATION";
        case HN4_ERR_TAMPERED:              return "ERR_TAMPERED";
        case HN4_ERR_TIME_PARADOX:          return "ERR_TIME_PARADOX";
        
        /* --- The Manifold --- */
        case HN4_ERR_BAD_SUPERBLOCK:        return "ERR_BAD_SUPERBLOCK";
        case HN4_ERR_VERSION_INCOMPAT:      return "ERR_VERSION_INCOMPAT";
        case HN4_ERR_NOMEM:                 return "ERR_NOMEM";
        case HN4_ERR_DMA_MAPPING:           return "ERR_DMA_MAPPING";
        case HN4_ERR_PROFILE_MISMATCH:      return "ERR_PROFILE_MISMATCH";
        case HN4_ERR_PICO_LIMIT:            return "ERR_PICO_LIMIT";
        case HN4_ERR_ENDIAN_MISMATCH:       return "ERR_ENDIAN_MISMATCH";
        case HN4_ERR_INTERNAL_FAULT:        return "ERR_INTERNAL_FAULT";
        case HN4_ERR_GEOMETRY:              return "ERR_GEOMETRY";
        case HN4_ERR_INVALID_ARGUMENT:      return "ERR_INVALID_ARGUMENT";
        case HN4_ERR_UNINITIALIZED:         return "ERR_UNINITIALIZED";
        case HN4_ERR_EEXIST:                return "ERR_EEXIST";
        case HN4_ERR_COMPRESSION_INEFFICIENT: return "ERR_COMPRESSION_INEFFICIENT";
        case HN4_ERR_ALGO_UNKNOWN:          return "ERR_ALGO_UNKNOWN";
        case HN4_ERR_KEY_EXPIRED:           return "ERR_KEY_EXPIRED";
        
        default:                            return "ERR_UNKNOWN";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* HN4_ERRORS_H */