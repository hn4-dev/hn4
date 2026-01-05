/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4_errors.h
 * STATUS:      RATIFIED (v5.5)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ERROR PARADIGM: THE NEGATIVE MANIFOLD
 *  - 0      : The Singularity (Success)
 *  - > 0    : The Positive Manifold (Informational)
 *  - < 0    : The Negative Manifold (Hard Errors)
 */

#ifndef HN4_ERRORS_H
#define HN4_ERRORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type definition for results */
typedef int32_t hn4_result_t;

/* =========================================================================
 * 0. THE SINGULARITY (SUCCESS)
 * ========================================================================= */
#define HN4_OK                      0

/* 
 * POSITIVE MANIFOLD (Informational)
 * Used when an operation succeeded, but with caveats.
 * NOTE: HN4_IS_OK() returns true for these values.
 */
#define HN4_INFO_PENDING            1   /* Async Operation Queued */
#define HN4_INFO_HEALED             2   /* Spec 21.1: Read success, but Helix Repair triggered */
#define HN4_INFO_SPARSE             3   /* Spec 11.1: Read success, returned implicit Zeros (Holo-Lattice) */
#define HN4_INFO_HORIZON_FALLBACK   4   /* Spec 6.4: Allocation succeeded, but forced to D1.5 */
#define HN4_INFO_THAWED             5   /* Spec 20.5: Write succeeded, but forced decompression */

/* 
 * Helper Macros 
 * WARNING: HN4_IS_OK(x) is true for Positive Manifold codes (1..5).
 * If you require strict success (0), check (x == HN4_OK).
 */
#define HN4_IS_OK(x)    ((x) >= 0)
#define HN4_IS_ERR(x)   ((x) < 0)

/* =========================================================================
 * 1. THE VOID (SPACE & ALLOCATION) [-0x100 to -0x1FF]
 * ========================================================================= */
/* Spec 18.8: Physical storage exhausted. No trajectories available. */
#define HN4_ERR_ENOSPC              -0x100

/* Spec 6.4: D1 (Flux) is full, and D1.5 (Horizon) is also full. */
#define HN4_ERR_EVENT_HORIZON       -0x101

/* Spec 18.1: The Void Engine hit max k=12 collision limit. */
#define HN4_ERR_GRAVITY_COLLAPSE    -0x102

/* Spec 5.3: The Armored Bitmap in RAM failed ECC check. */
#define HN4_ERR_BITMAP_CORRUPT      -0x103

/* Spec 18.10: Allocation requested specific Fractal alignment but failed. */
#define HN4_ERR_ALIGNMENT_FAIL      -0x104

/* Hardware atomic operation (CAS) timed out under contention. */
#define HN4_ERR_ATOMICS_TIMEOUT     -0x105

/* Spec 13.2: ZNS Zone Append failed (Zone Full/Read-Only). */
#define HN4_ERR_ZONE_FULL           -0x106

/* Spec 16.2: Volume is marked VOL_PENDING_WIPE (The Nuke Protocol). */
#define HN4_ERR_WIPE_PENDING        -0x107


/* =========================================================================
 * 2. THE CORTEX (IDENTITY & LOOKUP) [-0x200 to -0x2FF]
 * ========================================================================= */
/* Spec 23.1: Anchor not found in D0 or Nano-Cortex Cache. */
#define HN4_ERR_NOT_FOUND           -0x200

/* Spec 18.4: Anchor exists but is marked HN4_FLAG_TOMBSTONE. */
#define HN4_ERR_TOMBSTONE           -0x201

/* Spec 11.1: Anchor ID mismatch (Seed ID vs Payload). Ghost detected. */
#define HN4_ERR_ID_MISMATCH         -0x202

/* Spec 8.4: Tag Query returned too many results for the buffer. */
#define HN4_ERR_TAG_OVERFLOW        -0x203

/* Spec 19.6: Name too long for Inline Buffer and Extensions full. */
#define HN4_ERR_NAME_TOO_LONG       -0x204


/* =========================================================================
 * 3. THE SOVEREIGN (SECURITY & PERMISSION) [-0x300 to -0x3FF]
 * ========================================================================= */
/* Spec 9.1: General Access Denied (No Key, No Tether). */
#define HN4_ERR_ACCESS_DENIED       -0x300

/* Spec 9.4: Operation rejected by PERM_IMMUTABLE flag. */
#define HN4_ERR_IMMUTABLE           -0x301

/* Spec 15.3: Ed25519 Signature verification failed. */
#define HN4_ERR_SIG_INVALID         -0x302

/* Spec 22.2: Tether has expired (Current Time > Tether.expiry_ts). */
#define HN4_ERR_TETHER_EXPIRED      -0x303

/* Spec 9.2: Operation requires Sovereign Key (Root). */
#define HN4_ERR_NOT_SOVEREIGN       -0x304

/* Spec 16.1: Volume is in VOL_LOCKED state (Ransomware/Lockdown). */
#define HN4_ERR_VOLUME_LOCKED       -0x305

/* Spec 9.6: Audit Chronicle Write Failed (Must fail op). */
#define HN4_ERR_AUDIT_FAILURE       -0x306


/* =========================================================================
 * 4. THE HELIX (INTEGRITY & HARDWARE) [-0x400 to -0x4FF]
 * ========================================================================= */
/* Spec 21.2: Generic Hardware IO Error (EIO). */
#define HN4_ERR_HW_IO               -0x400

/* Spec 21.1: Data CRC32C mismatch. Auto-Medic failed to heal. */
#define HN4_ERR_DATA_ROT            -0x401

/* Spec 11.5: Reed-Solomon reconstruction failed (Too many bad shards). */
#define HN4_ERR_PARITY_BROKEN       -0x402

/* Spec 25.1: Block Header well_id mismatch (Phantom Read). */
#define HN4_ERR_PHANTOM_BLOCK       -0x403

/* Spec 20.1: Decompression (LZ4/ZSTD) failed (Corrupt Payload). */
#define HN4_ERR_DECOMPRESS_FAIL     -0x404

/* Spec 10.5: Drive Temperature critical. Operation throttled/aborted. */
#define HN4_ERR_THERMAL_CRITICAL    -0x405

/* Spec 21.5: CPU Integrity Check failed (RAM/ALU unstable). */
#define HN4_ERR_CPU_INSANITY        -0x406

/* Spec 21.3: Encrypted block corruption */
#define HN4_ERR_ENCRYPTED_ROT       -0x408 

/* Spec 16.1: Volume state is VOL_TOXIC. Media is dying. */
#define HN4_ERR_MEDIA_TOXIC         -0x407

/* Add to Section 6: THE MANIFOLD */
#define HN4_ERR_UNINITIALIZED       -0x60A  /* Operation requires zeroed/init memory */

/* =========================================================================
 * 5. THE CHRONO-SPHERE (TIME & STATE) [-0x500 to -0x5FF]
 * ========================================================================= */
/* Spec 23.2: Generation Counter mismatch (Phantom Write Defense). */
#define HN4_ERR_GENERATION_SKEW     -0x500

/* Spec 24.2: Requested Epoch ID not found in the Ring (Too old). */
#define HN4_ERR_EPOCH_LOST          -0x501

/* Spec 24.1: Snapshot ID not found or invalid. */
#define HN4_ERR_SNAPSHOT_INVALID    -0x502

/* Spec 22.1: Reflink/Dedupe attempted (Quantum Exclusion Principle). */
#define HN4_ERR_QUANTUM_VIOLATION   -0x503

/* Spec 22.3: System Clock is behind Last Mount Time. */
#define HN4_ERR_TIME_DILATION       -0x504

/* Spec 24.6: Chronicle indicates Tampering (Seq ID gap). */
#define HN4_ERR_TAMPERED            -0x505

/* Spec 24.4: Attempted to write to a Read-Only Historical View. */
#define HN4_ERR_TIME_PARADOX        -0x506


/* =========================================================================
 * 6. THE MANIFOLD (SYSTEM & DRIVER) [-0x600 to -0x6FF]
 * ========================================================================= */
/* Spec 12.1: Superblock Magic Invalid. */
#define HN4_ERR_BAD_SUPERBLOCK      -0x600

/* Spec 16.5: Feature flag in SB.incompat_flags not supported. */
#define HN4_ERR_VERSION_INCOMPAT    -0x601

/* Spec 5.2: Not enough Kernel RAM for Nano-Cortex. */
#define HN4_ERR_NOMEM               -0x602

/* Spec 10.2: GPU Direct (Tensor Tunnel) Setup Failed. */
#define HN4_ERR_DMA_MAPPING         -0x603

/* Spec 10.4: Operation invalid for this Device Profile (e.g. ZNS op on HDD). */
#define HN4_ERR_PROFILE_MISMATCH    -0x604

/* Spec 14.1: File too large for 32-bit Pico Profile. */
#define HN4_ERR_PICO_LIMIT          -0x605

/* Spec 2.2: Endianness check failed (sb.endian_tag mismatch). */
#define HN4_ERR_ENDIAN_MISMATCH     -0x606

/* Driver Internal Logic Error (Bug/Assert). */
#define HN4_ERR_INTERNAL_FAULT      -0x607

/* Spec 13.4: Drive capacity violates Profile Min/Max limits. */
#define HN4_ERR_GEOMETRY            -0x608

#define HN4_ERR_INVALID_ARGUMENT    -0x609

#define HN4_ERR_EEXIST    -0x610

/* =========================================================================
 * 7. UTILITY FUNCTIONS
 * ========================================================================= */

/**
 * hn4_strerror()
 * Returns a static string representation of the error code.
 * Useful for Triage Logs (Spec 21.4).
 */
static inline const char* hn4_strerror(hn4_result_t res) {
    switch (res) {
        case HN4_OK:                      return "SUCCESS";
        case HN4_INFO_PENDING:            return "PENDING";
        case HN4_INFO_HEALED:             return "HEALED_VIA_HELIX";
        case HN4_INFO_SPARSE:             return "SPARSE_READ";
        case HN4_INFO_HORIZON_FALLBACK:   return "HORIZON_FALLBACK";
        case HN4_INFO_THAWED:             return "THAWED";
        
        case HN4_ERR_ENOSPC:              return "ERR_ENOSPC";
        case HN4_ERR_EVENT_HORIZON:       return "ERR_EVENT_HORIZON";
        case HN4_ERR_GRAVITY_COLLAPSE:    return "ERR_GRAVITY_COLLAPSE";
        case HN4_ERR_BITMAP_CORRUPT:      return "ERR_BITMAP_CORRUPT";
        case HN4_ERR_ALIGNMENT_FAIL:      return "ERR_ALIGNMENT_FAIL";
        case HN4_ERR_ATOMICS_TIMEOUT:     return "ERR_ATOMICS_TIMEOUT";
        case HN4_ERR_ZONE_FULL:           return "ERR_ZONE_FULL";
        case HN4_ERR_WIPE_PENDING:        return "ERR_WIPE_PENDING";
        
        case HN4_ERR_NOT_FOUND:           return "ERR_NOT_FOUND";
        case HN4_ERR_TOMBSTONE:           return "ERR_TOMBSTONE";
        case HN4_ERR_ID_MISMATCH:         return "ERR_ID_MISMATCH";
        case HN4_ERR_TAG_OVERFLOW:        return "ERR_TAG_OVERFLOW";
        case HN4_ERR_NAME_TOO_LONG:       return "ERR_NAME_TOO_LONG";
        
        case HN4_ERR_ACCESS_DENIED:       return "ERR_ACCESS_DENIED";
        case HN4_ERR_IMMUTABLE:           return "ERR_IMMUTABLE";
        case HN4_ERR_SIG_INVALID:         return "ERR_SIG_INVALID";
        case HN4_ERR_TETHER_EXPIRED:      return "ERR_TETHER_EXPIRED";
        case HN4_ERR_NOT_SOVEREIGN:       return "ERR_NOT_SOVEREIGN";
        case HN4_ERR_VOLUME_LOCKED:       return "ERR_VOLUME_LOCKED";
        case HN4_ERR_AUDIT_FAILURE:       return "ERR_AUDIT_FAILURE";
        
        case HN4_ERR_HW_IO:               return "ERR_HW_IO";
        case HN4_ERR_DATA_ROT:            return "ERR_DATA_ROT";
        case HN4_ERR_PARITY_BROKEN:       return "ERR_PARITY_BROKEN";
        case HN4_ERR_PHANTOM_BLOCK:       return "ERR_PHANTOM_BLOCK";
        case HN4_ERR_DECOMPRESS_FAIL:     return "ERR_DECOMPRESS_FAIL";
        case HN4_ERR_THERMAL_CRITICAL:    return "ERR_THERMAL_CRITICAL";
        case HN4_ERR_CPU_INSANITY:        return "ERR_CPU_INSANITY";
        case HN4_ERR_MEDIA_TOXIC:         return "ERR_MEDIA_TOXIC";
        
        case HN4_ERR_GENERATION_SKEW:     return "ERR_GENERATION_SKEW";
        case HN4_ERR_EPOCH_LOST:          return "ERR_EPOCH_LOST";
        case HN4_ERR_SNAPSHOT_INVALID:    return "ERR_SNAPSHOT_INVALID";
        case HN4_ERR_QUANTUM_VIOLATION:   return "ERR_QUANTUM_VIOLATION";
        case HN4_ERR_TIME_DILATION:       return "ERR_TIME_DILATION";
        case HN4_ERR_TAMPERED:            return "ERR_TAMPERED";
        case HN4_ERR_TIME_PARADOX:        return "ERR_TIME_PARADOX";
        
        case HN4_ERR_BAD_SUPERBLOCK:      return "ERR_BAD_SUPERBLOCK";
        case HN4_ERR_VERSION_INCOMPAT:    return "ERR_VERSION_INCOMPAT";
        case HN4_ERR_NOMEM:               return "ERR_NOMEM";
        case HN4_ERR_DMA_MAPPING:         return "ERR_DMA_MAPPING";
        case HN4_ERR_PROFILE_MISMATCH:    return "ERR_PROFILE_MISMATCH";
        case HN4_ERR_PICO_LIMIT:          return "ERR_PICO_LIMIT";
        case HN4_ERR_ENDIAN_MISMATCH:     return "ERR_ENDIAN_MISMATCH";
        case HN4_ERR_INTERNAL_FAULT:      return "ERR_INTERNAL_FAULT";
        
        default:                          return "ERR_UNKNOWN";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* HN4_ERRORS_H */